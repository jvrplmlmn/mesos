#include <arpa/inet.h>

#include <stdint.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <queue>
#include <string>
#include <vector>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/internal.hpp>
#include <process/owned.hpp>
#include <process/socket.hpp>

#include <stout/foreach.hpp>
#include <stout/ip.hpp>
#include <stout/lambda.hpp>
#include <stout/net.hpp>
#include <stout/nothing.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "decoder.hpp"

using std::deque;
using std::map;
using std::queue;
using std::string;
using std::vector;

using process::http::Request;
using process::http::Response;

using process::network::Address;
using process::network::Socket;

namespace process {
namespace http {


hashmap<uint16_t, string> statuses;


void initialize()
{
  statuses[100] = "100 Continue";
  statuses[101] = "101 Switching Protocols";
  statuses[200] = "200 OK";
  statuses[201] = "201 Created";
  statuses[202] = "202 Accepted";
  statuses[203] = "203 Non-Authoritative Information";
  statuses[204] = "204 No Content";
  statuses[205] = "205 Reset Content";
  statuses[206] = "206 Partial Content";
  statuses[300] = "300 Multiple Choices";
  statuses[301] = "301 Moved Permanently";
  statuses[302] = "302 Found";
  statuses[303] = "303 See Other";
  statuses[304] = "304 Not Modified";
  statuses[305] = "305 Use Proxy";
  statuses[307] = "307 Temporary Redirect";
  statuses[400] = "400 Bad Request";
  statuses[401] = "401 Unauthorized";
  statuses[402] = "402 Payment Required";
  statuses[403] = "403 Forbidden";
  statuses[404] = "404 Not Found";
  statuses[405] = "405 Method Not Allowed";
  statuses[406] = "406 Not Acceptable";
  statuses[407] = "407 Proxy Authentication Required";
  statuses[408] = "408 Request Time-out";
  statuses[409] = "409 Conflict";
  statuses[410] = "410 Gone";
  statuses[411] = "411 Length Required";
  statuses[412] = "412 Precondition Failed";
  statuses[413] = "413 Request Entity Too Large";
  statuses[414] = "414 Request-URI Too Large";
  statuses[415] = "415 Unsupported Media Type";
  statuses[416] = "416 Requested range not satisfiable";
  statuses[417] = "417 Expectation Failed";
  statuses[500] = "500 Internal Server Error";
  statuses[501] = "501 Not Implemented";
  statuses[502] = "502 Bad Gateway";
  statuses[503] = "503 Service Unavailable";
  statuses[504] = "504 Gateway Time-out";
  statuses[505] = "505 HTTP Version not supported";
}


bool Request::accepts(const string& encoding) const
{
  // See RFC 2616, section 14.3 for the details.
  Option<string> accepted = headers.get("Accept-Encoding");

  if (accepted.isNone()) {
    return false;
  }

  // Remove spaces and tabs for easier parsing.
  accepted = strings::remove(accepted.get(), " ");
  accepted = strings::remove(accepted.get(), "\t");
  accepted = strings::remove(accepted.get(), "\n");

  // From RFC 2616:
  // 1. If the content-coding is one of the content-codings listed in
  //    the Accept-Encoding field, then it is acceptable, unless it is
  //    accompanied by a qvalue of 0. (As defined in section 3.9, a
  //    qvalue of 0 means "not acceptable.")
  // 2. The special "*" symbol in an Accept-Encoding field matches any
  //    available content-coding not explicitly listed in the header
  //    field.

  // First we'll look for the encoding specified explicitly, then '*'.
  vector<string> candidates;
  candidates.push_back(encoding);      // Rule 1.
  candidates.push_back("*");           // Rule 2.

  foreach (const string& candidate, candidates) {
    // Is the candidate one of the accepted encodings?
    foreach (const string& _encoding, strings::tokenize(accepted.get(), ",")) {
      if (strings::startsWith(_encoding, candidate)) {
        // Is there a 0 q value? Ex: 'gzip;q=0.0'.
        const map<string, vector<string> >& values =
          strings::pairs(_encoding, ";", "=");

        // Look for { "q": ["0"] }.
        if (values.count("q") == 0 || values.find("q")->second.size() != 1) {
          // No q value, or malformed q value.
          return true;
        }

        // Is the q value > 0?
        Try<double> value = numify<double>(values.find("q")->second[0]);
        return value.isSome() && value.get() > 0;
      }
    }
  }

  // NOTE: 3 and 4 are partially ignored since we can only provide gzip.
  // 3. If multiple content-codings are acceptable, then the acceptable
  //    content-coding with the highest non-zero qvalue is preferred.
  // 4. The "identity" content-coding is always acceptable, unless
  //    specifically refused because the Accept-Encoding field includes
  //    "identity;q=0", or because the field includes "*;q=0" and does
  //    not explicitly include the "identity" content-coding. If the
  //    Accept-Encoding field-value is empty, then only the "identity"
  //    encoding is acceptable.
  return false;
}


Pipe::Reader Pipe::reader() const
{
  return Pipe::Reader(data);
}


Pipe::Writer Pipe::writer() const
{
  return Pipe::Writer(data);
}


Future<string> Pipe::Reader::read()
{
  Future<string> future;

  process::internal::acquire(&data->lock);
  {
    if (data->readEnd == CLOSED) {
      future = Failure("closed");
    } else if (!data->writes.empty()) {
      future = data->writes.front();
      data->writes.pop();
    } else if (data->writeEnd == CLOSED) {
      future = ""; // End-of-file.
    } else {
      data->reads.push(Owned<Promise<string>>(new Promise<string>()));
      future = data->reads.back()->future();
    }
  }
  process::internal::release(&data->lock);

  return future;
}


bool Pipe::Reader::close()
{
  bool closed = false;
  bool notify = false;
  queue<Owned<Promise<string>>> reads;

  process::internal::acquire(&data->lock);
  {
    if (data->readEnd == OPEN) {
      // Throw away outstanding data.
      while (!data->writes.empty()) {
        data->writes.pop();
      }

      // Extract the pending reads so we can fail them.
      std::swap(data->reads, reads);

      closed = true;
      data->readEnd = CLOSED;

      // Notify if write-end is still open!
      notify = data->writeEnd == OPEN;
    }
  }
  process::internal::release(&data->lock);

  // NOTE: We transition the promises outside the critical section
  // to avoid triggering callbacks that try to reacquire the lock.
  if (closed) {
    while (!reads.empty()) {
      reads.front()->fail("closed");
      reads.pop();
    }

    if (notify) {
      data->readerClosure.set(Nothing());
    }
  }

  return closed;
}


bool Pipe::Writer::write(const string& s)
{
  bool written = false;
  Owned<Promise<string>> read;

  process::internal::acquire(&data->lock);
  {
    // Ignore writes if either end of the pipe is closed!
    if (data->writeEnd == OPEN && data->readEnd == OPEN) {
      // Don't bother surfacing empty writes to the readers.
      if (!s.empty()) {
        if (data->reads.empty()) {
          data->writes.push(s);
        } else {
          read = data->reads.front();
          data->reads.pop();
        }
      }
      written = true;
    }
  }
  process::internal::release(&data->lock);

  // NOTE: We set the promise outside the critical section to avoid
  // triggering callbacks that try to reacquire the lock.
  if (read.get() != NULL) {
    read->set(s);
  }

  return written;
}


bool Pipe::Writer::close()
{
  bool closed = false;
  queue<Owned<Promise<string>>> reads;

  process::internal::acquire(&data->lock);
  {
    if (data->writeEnd == OPEN) {
      // Extract all the pending reads so we can complete them.
      std::swap(data->reads, reads);

      data->writeEnd = CLOSED;
      closed = true;
    }
  }
  process::internal::release(&data->lock);

  // NOTE: We set the promises outside the critical section to avoid
  // triggering callbacks that try to reacquire the lock.
  while (!reads.empty()) {
    reads.front()->set(string("")); // End-of-file.
    reads.pop();
  }

  return closed;
}


Future<Nothing> Pipe::Writer::readerClosed()
{
  return data->readerClosure.future();
}


namespace query {

Try<hashmap<std::string, std::string>> decode(const std::string& query)
{
  hashmap<std::string, std::string> result;

  const std::vector<std::string>& tokens = strings::tokenize(query, ";&");
  foreach (const std::string& token, tokens) {
    const std::vector<std::string>& pairs = strings::split(token, "=", 2);
    if (pairs.size() == 0) {
      continue;
    }

    Try<std::string> key = http::decode(pairs[0]);
    if (key.isError()) {
      return Error(key.error());
    }

    if (pairs.size() == 2) {
      Try<std::string> value = http::decode(pairs[1]);
      if (value.isError()) {
        return Error(value.error());
      }
      result[key.get()] = value.get();

    } else if (pairs.size() == 1) {
      result[key.get()] = "";
    }
  }

  return result;
}


std::string encode(const hashmap<std::string, std::string>& query)
{
  std::string output;

  foreachpair (const std::string& key, const std::string& value, query) {
    output += http::encode(key);
    if (!value.empty()) {
      output += "=" + http::encode(value);
    }
    output += '&';
  }
  return strings::remove(output, "&", strings::SUFFIX);
}

} // namespace query {

namespace internal {

Future<Response> decode(const string& buffer)
{
  ResponseDecoder decoder;
  deque<Response*> responses = decoder.decode(buffer.c_str(), buffer.length());

  if (decoder.failed() || responses.empty()) {
    for (size_t i = 0; i < responses.size(); ++i) {
      delete responses[i];
    }
    return Failure("Failed to decode HTTP response:\n" + buffer + "\n");
  } else if (responses.size() > 1) {
    PLOG(ERROR) << "Received more than 1 HTTP Response";
  }

  Response response = *responses[0];
  for (size_t i = 0; i < responses.size(); ++i) {
    delete responses[i];
  }

  return response;
}


// Forward declaration.
Future<Response> _request(
    Socket socket,
    const Address& address,
    const URL& url,
    const string& method,
    const Option<hashmap<string, string>>& _headers,
    const Option<string>& body,
    const Option<string>& contentType);


Future<Response> request(
    const URL& url,
    const string& method,
    const Option<hashmap<string, string>>& headers,
    const Option<string>& body,
    const Option<string>& contentType)
{
  if (url.scheme != "http") {
    return Failure("Unsupported URL scheme");
  }

  Try<Socket> create = Socket::create();

  if (create.isError()) {
    return Failure("Failed to create socket: " + create.error());
  }

  Socket socket = create.get();

  Address address;

  if (url.ip.isSome()) {
    address.ip = url.ip.get();
  } else if (url.domain.isNone()) {
    return Failure("Missing URL domain or IP");
  } else {
    Try<net::IP> ip = net::getIP(url.domain.get(), AF_INET);

    if (ip.isError()) {
      return Failure("Failed to determine IP of domain '" +
                     url.domain.get() + "': " + ip.error());
    }

    address.ip = ip.get();
  }

  address.port = url.port;

  return socket.connect(address)
    .then(lambda::bind(&_request,
                       socket,
                       address,
                       url,
                       method,
                       headers,
                       body,
                       contentType));
}


Future<Response> _request(
    Socket socket,
    const Address& address,
    const URL& url,
    const string& method,
    const Option<hashmap<string, string>>& _headers,
    const Option<string>& body,
    const Option<string>& contentType)
{
  std::ostringstream out;

  out << method << " /" << strings::remove(url.path, "/", strings::PREFIX);

  if (!url.query.empty()) {
    // Convert the query to a string that we join via '=' and '&'.
    vector<string> query;

    foreachpair (const string& key, const string& value, url.query) {
      query.push_back(key + "=" + value);
    }

    out << "?" << strings::join("&", query);
  }

  if (url.fragment.isSome()) {
    out << "#" << url.fragment.get();
  }

  out << " HTTP/1.1\r\n";

  // Set up the headers as necessary.
  hashmap<string, string> headers;

  if (_headers.isSome()) {
    headers = _headers.get();
  }

  // Need to specify the 'Host' header.
  headers["Host"] = stringify(address);

  // Tell the server to close the connection when it's done.
  headers["Connection"] = "close";

  // Overwrite Content-Type if necessary.
  if (contentType.isSome()) {
    headers["Content-Type"] = contentType.get();
  }

  // Make sure the Content-Length is set correctly if necessary.
  if (body.isSome()) {
    headers["Content-Length"] = stringify(body.get().length());
  }

  // Emit the headers.
  foreachpair (const string& key, const string& value, headers) {
    out << key << ": " << value << "\r\n";
  }

  out << "\r\n";

  if (body.isSome()) {
    out << body.get();
  }

  // Need to disambiguate the Socket::recv for binding below.
  Future<string> (Socket::*recv)(const Option<ssize_t>&) = &Socket::recv;

  // TODO(bmahler): For efficiency, this should properly use the
  // ResponseDecoder when reading, rather than parsing the full string
  // response.
  return socket.send(out.str())
    .then(lambda::function<Future<string>(void)>(
              lambda::bind(recv, socket, -1)))
    .then(lambda::bind(&internal::decode, lambda::_1));
}

} // namespace internal {


Future<Response> get(
    const URL& url,
    const Option<hashmap<string, string>>& headers)
{
  return internal::request(url, "GET", headers, None(), None());
}


Future<Response> put(
    const URL& url,
    const Option<hashmap<string, string>>& headers,
    const Option<string>& body,
    const Option<string>& contentType)
{
  if (body.isNone() && contentType.isSome()) {
    return Failure("Attempted to do a PUT with a Content-Type but no body");
  }

  return internal::request(url, "PUT", headers, body, contentType);
}


Future<Response> post(
    const URL& url,
    const Option<hashmap<string, string>>& headers,
    const Option<string>& body,
    const Option<string>& contentType)
{
  if (body.isNone() && contentType.isSome()) {
    return Failure("Attempted to do a POST with a Content-Type but no body");
  }

  return internal::request(url, "POST", headers, body, contentType);
}


Future<Response> get(
    const UPID& upid,
    const Option<string>& path,
    const Option<string>& query,
    const Option<hashmap<string, string>>& headers)
{
  URL url("http", net::IP(upid.address.ip), upid.address.port, upid.id);

  if (path.isSome()) {
    // TODO(benh): Get 'query' and/or 'fragment' out of 'path'.
    url.path = strings::join("/", url.path, path.get());
  }

  if (query.isSome()) {
    Try<hashmap<string, string>> decode = http::query::decode(
        strings::remove(query.get(), "?", strings::PREFIX));

    if (decode.isError()) {
      return Failure("Failed to decode HTTP query string: " + decode.error());
    }

    url.query = decode.get();
  }

  return get(url, headers);
}


Future<Response> post(
    const UPID& upid,
    const Option<string>& path,
    const Option<hashmap<string, string>>& headers,
    const Option<string>& body,
    const Option<string>& contentType)
{
  URL url("http", net::IP(upid.address.ip), upid.address.port, upid.id);

  if (path.isSome()) {
    // TODO(benh): Get 'query' and/or 'fragment' out of 'path'.
    url.path = strings::join("/", url.path, path.get());
  }

  return post(url, headers, body, contentType);
}

} // namespace http {
} // namespace process {
