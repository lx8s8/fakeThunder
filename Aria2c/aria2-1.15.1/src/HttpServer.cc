/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2009 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#include "HttpServer.h"

#include <sstream>

#include "HttpHeader.h"
#include "SocketCore.h"
#include "HttpHeaderProcessor.h"
#include "DlAbortEx.h"
#include "message.h"
#include "util.h"
#include "LogFactory.h"
#include "Logger.h"
#include "base64.h"
#include "a2functional.h"
#include "fmt.h"
#include "SocketRecvBuffer.h"
#include "TimeA2.h"
#include "array_fun.h"

namespace aria2 {

HttpServer::HttpServer
(const SharedHandle<SocketCore>& socket,
 DownloadEngine* e)
 : socket_(socket),
   socketRecvBuffer_(new SocketRecvBuffer(socket_)),
   socketBuffer_(socket),
   e_(e),
   headerProcessor_(new HttpHeaderProcessor()),
   keepAlive_(true),
   gzip_(false),
   acceptsPersistentConnection_(true),
   acceptsGZip_(false)
{}

HttpServer::~HttpServer() {}

namespace {
const char* getStatusString(int status)
{
  switch(status) {
  case 100: return "100 Continue";
  case 101: return "101 Switching Protocols";
  case 200: return "200 OK";
  case 201: return "201 Created";
  case 202: return "202 Accepted";
  case 203: return "203 Non-Authoritative Information";
  case 204: return "204 No Content";
  case 205: return "205 Reset Content";
  case 206: return "206 Partial Content";
  case 300: return "300 Multiple Choices";
  case 301: return "301 Moved Permanently";
  case 302: return "302 Found";
  case 303: return "303 See Other";
  case 304: return "304 Not Modified";
  case 305: return "305 Use Proxy";
    // case 306: return "306 (Unused)";
  case 307: return "307 Temporary Redirect";
  case 400: return "400 Bad Request";
  case 401: return "401 Unauthorized";
  case 402: return "402 Payment Required";
  case 403: return "403 Forbidden";
  case 404: return "404 Not Found";
  case 405: return "405 Method Not Allowed";
  case 406: return "406 Not Acceptable";
  case 407: return "407 Proxy Authentication Required";
  case 408: return "408 Request Timeout";
  case 409: return "409 Conflict";
  case 410: return "410 Gone";
  case 411: return "411 Length Required";
  case 412: return "412 Precondition Failed";
  case 413: return "413 Request Entity Too Large";
  case 414: return "414 Request-URI Too Long";
  case 415: return "415 Unsupported Media Type";
  case 416: return "416 Requested Range Not Satisfiable";
  case 417: return "417 Expectation Failed";
    // RFC 2817 defines 426 status code.
  case 426: return "426 Upgrade Required";
  case 500: return "500 Internal Server Error";
  case 501: return "501 Not Implemented";
  case 502: return "502 Bad Gateway";
  case 503: return "503 Service Unavailable";
  case 504: return "504 Gateway Timeout";
  case 505: return "505 HTTP Version Not Supported";
  default: return "";
  }
}
} // namespace

SharedHandle<HttpHeader> HttpServer::receiveRequest()
{
  if(socketRecvBuffer_->bufferEmpty()) {
    if(socketRecvBuffer_->recv() == 0 &&
       !socket_->wantRead() && !socket_->wantWrite()) {
      throw DL_ABORT_EX(EX_EOF_FROM_PEER);
    }
  }
  headerProcessor_->update(socketRecvBuffer_->getBuffer(),
                           socketRecvBuffer_->getBufferLength());
  if(headerProcessor_->eoh()) {
    SharedHandle<HttpHeader> header = headerProcessor_->getHttpRequestHeader();
    size_t putbackDataLength = headerProcessor_->getPutBackDataLength();
    A2_LOG_INFO(fmt("HTTP Server received request\n%s",
                    headerProcessor_->getHeaderString().c_str()));
    socketRecvBuffer_->shiftBuffer
      (socketRecvBuffer_->getBufferLength()-putbackDataLength);
    lastRequestHeader_ = header;
    lastBody_.clear();
    lastBody_.str("");
    lastContentLength_ =
      lastRequestHeader_->findAsLLInt(HttpHeader::CONTENT_LENGTH);
    if(lastContentLength_ < 0) {
      throw DL_ABORT_EX("Content-Length must be positive.");
    }
    headerProcessor_->clear();

    const std::string& connection =
      lastRequestHeader_->find(HttpHeader::CONNECTION);
    acceptsPersistentConnection_ =
      util::strifind(connection.begin(),
                     connection.end(),
                     HttpHeader::CLOSE.begin(),
                     HttpHeader::CLOSE.end()) == connection.end() &&
      (lastRequestHeader_->getVersion() == HttpHeader::HTTP_1_1 ||
       util::strifind(connection.begin(),
                      connection.end(),
                      HttpHeader::KEEP_ALIVE.begin(),
                      HttpHeader::KEEP_ALIVE.end()) != connection.end());

    std::vector<Scip> acceptEncodings;
    const std::string& acceptEnc =
      lastRequestHeader_->find(HttpHeader::ACCEPT_ENCODING);
    util::splitIter(acceptEnc.begin(), acceptEnc.end(),
                    std::back_inserter(acceptEncodings), ',', true);
    acceptsGZip_ = false;
    for(std::vector<Scip>::const_iterator i = acceptEncodings.begin(),
          eoi = acceptEncodings.end(); i != eoi; ++i) {
      if(util::strieq((*i).first, (*i).second,
                      HttpHeader::GZIP.begin(), HttpHeader::GZIP.end())) {
        acceptsGZip_ = true;
        break;
      }
    }
    return header;
  } else {
    socketRecvBuffer_->clearBuffer();
    return SharedHandle<HttpHeader>();
  }
}

bool HttpServer::receiveBody()
{
  if(lastContentLength_ == 0) {
    return true;
  }
  if(socketRecvBuffer_->bufferEmpty()) {
    if(socketRecvBuffer_->recv() == 0 &&
       !socket_->wantRead() && !socket_->wantWrite()) {
      throw DL_ABORT_EX(EX_EOF_FROM_PEER);
    }
  }
  size_t length =
    std::min(socketRecvBuffer_->getBufferLength(),
             static_cast<size_t>(lastContentLength_-lastBody_.tellg()));
  lastBody_.write(reinterpret_cast<const char*>(socketRecvBuffer_->getBuffer()),
                  length);
  socketRecvBuffer_->shiftBuffer(length);
  return lastContentLength_ == lastBody_.tellp();
}

std::string HttpServer::getBody() const
{
  return lastBody_.str();
}

const std::string& HttpServer::getMethod() const
{
  return lastRequestHeader_->getMethod();
}

const std::string& HttpServer::getRequestPath() const
{
  return lastRequestHeader_->getRequestPath();
}

void HttpServer::feedResponse(std::string& text, const std::string& contentType)
{
  feedResponse(200, "", text, contentType);
}

void HttpServer::feedResponse(int status,
                              const std::string& headers,
                              const std::string& text,
                              const std::string& contentType)
{
  std::string httpDate = Time().toHTTPDate();
  std::string header= fmt("HTTP/1.1 %s\r\n"
                          "Date: %s\r\n"
                          "Content-Length: %lu\r\n"
                          "Expires: %s\r\n"
                          "Cache-Control: no-cache\r\n",
                          getStatusString(status),
                          httpDate.c_str(),
                          static_cast<unsigned long>(text.size()),
                          httpDate.c_str());
  if(!contentType.empty()) {
    header += "Content-Type: ";
    header += contentType;
    header += "\r\n";
  }
  if(!allowOrigin_.empty()) {
    header += "Access-Control-Allow-Origin: ";
    header += allowOrigin_;
    header += "\r\n";
  }
  if(supportsGZip()) {
    header += "Content-Encoding: gzip\r\n";
  }
  if(!supportsPersistentConnection()) {
    header += "Connection: close\r\n";
  }
  header += headers;
  header += "\r\n";
  A2_LOG_DEBUG(fmt("HTTP Server sends response:\n%s", header.c_str()));
  socketBuffer_.pushStr(header);
  socketBuffer_.pushStr(text);
}

void HttpServer::feedUpgradeResponse(const std::string& protocol,
                                     const std::string& headers)
{
  std::string header= fmt("HTTP/1.1 101 Switching Protocols\r\n"
                          "Upgrade: %s\r\n"
                          "Connection: Upgrade\r\n"
                          "%s"
                          "\r\n",
                          protocol.c_str(),
                          headers.c_str());
  A2_LOG_DEBUG(fmt("HTTP Server sends upgrade response:\n%s", header.c_str()));
  socketBuffer_.pushStr(header);
}

ssize_t HttpServer::sendResponse()
{
  return socketBuffer_.send();
}

bool HttpServer::sendBufferIsEmpty() const
{
  return socketBuffer_.sendBufferIsEmpty();
}

bool HttpServer::authenticate()
{
  if(username_.empty()) {
    return true;
  }

  const std::string& authHeader =
    lastRequestHeader_->find(HttpHeader::AUTHORIZATION);
  if(authHeader.empty()) {
    return false;
  }
  std::pair<Scip, Scip> p;
  util::divide(p, authHeader.begin(), authHeader.end(), ' ');
  if(!util::streq(p.first.first, p.first.second, "Basic")) {
    return false;
  }
  std::string userpass = base64::decode(p.second.first, p.second.second);
  std::pair<Sip, Sip> up;
  util::divide(up, userpass.begin(), userpass.end(), ':');
  return util::streq(up.first.first, up.first.second,
                     username_.begin(), username_.end()) &&
    util::streq(up.second.first, up.second.second,
                password_.begin(), password_.end());
}

void HttpServer::setUsernamePassword
(const std::string& username, const std::string& password)
{
  username_ = username;
  password_ = password;
}

} // namespace aria2
