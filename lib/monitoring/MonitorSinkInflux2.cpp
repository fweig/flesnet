// SPDX-License-Identifier: GPL-3.0-only
// (C) Copyright 2021 GSI Helmholtzzentrum für Schwerionenforschung
// Original author: Walter F.J. Mueller <w.f.j.mueller@gsi.de>

#include "MonitorSinkInflux2.hpp"

#include "Monitor.hpp"
#include <stdexcept>

#include "fmt/format.h"

// define needed for Boost 1.67 in Debian Buster to avoid a missing
// boost::system::system_category() symbol. That's apparently default since
// Boost 1.69, so Boost 1.71 (Ub focal) and 1.74 (Debian Bullseye work fine
// without. See https://stackoverflow.com/questions/9723793/
#define BOOST_ERROR_CODE_HEADER_ONLY
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <iostream>
#include <regex>

#include <stdlib.h>

namespace cbm {
using tcp = boost::asio::ip::tcp;     // from <boost/asio/ip/tcp.hpp>
namespace http = boost::beast::http;  // from <boost/beast/http.hpp>
using namespace std::string_literals; // for ""s

// some constants
static const size_t kSendChunkSize = 2'000'000ull; // send chunk size

/*! \class MonitorSinkInflux2
  \brief Monitor sink - concrete sink for InfluxDB V2 output

  Will transfer all queued metrics to the InfluxDB V2 instance and database
  specified at construction time. It also write periodically some
  self-monitoring data as Metric to measurement "Monitor" with the fields
  - `points`: number of metrics in last period
  - `tags`: total number of tags in all metrics in last period
  - `fields`: total number of fields in all metrics in last period
  - `sends`: number of HTTP post requests in last period
  - `bytes`: total number bytes written in last period
  - `sndtime`: total elapsed time spend in HTTP post requests (in s)
*/

//-----------------------------------------------------------------------------
/*! \brief Constructor
  \param monitor back reference to Monitor
  \param path write endpoint as `host:[port]:[bucket]:[token]`
  \throws std::runtime_error if `path` does not contain 4 fields
  \throws std::runtime_error if `token` in `path` is empty and CBM_INFLUX_TOKEN
  undefined

  Write metrics to an InfluxDB V2 accessed via HTTP and an endpoint defined
  by `path`:
  - `host`: host name of server
  - `port`: port number of influxdb service (default: '8086')
  - `bucket`: Influx bucket name (default: 'cbm')
  - `token`: Influx access token. If empty taken from the environment
             variable `CBM_INFLUX_TOKEN`

  The sink uses the V2 API `/api/v2/write` endpoint. The organisation is
  hardcoded to "CBM" via `?org=CBM`.
 */

MonitorSinkInflux2::MonitorSinkInflux2(Monitor& monitor,
                                       const std::string& path)
    : MonitorSink(monitor, path) {
  std::regex re_path(R"(^(.+?):([0-9]*?):(.*?):(.*)$)");
  std::smatch match;
  if (!std::regex_search(path.begin(), path.end(), match, re_path))
    throw std::runtime_error(
        fmt::format("MonitorSinkInflux2::ctor:"
                    " path not host:[port]:[bucket]:[token] '{}'",
                    path));
  fHost = match[1].str();
  fPort = match[2].str();
  fBucket = match[3].str();
  fToken = match[4].str();
  if (fPort.size() == 0)
    fPort = "8086";
  if (fBucket.size() == 0)
    fBucket = "cbm";
  if (fToken.size() == 0) {
    auto pchar = ::getenv("CBM_INFLUX_TOKEN");
    if (pchar == nullptr)
      throw std::runtime_error(
          "MonitorSinkInflux2::ctor:"
          " no token given and CBM_INFLUX_TOKEN not defined");
    fToken = std::string(pchar);
  }
}

//-----------------------------------------------------------------------------
/*! \brief Process a vector of metrics
 */

void MonitorSinkInflux2::ProcessMetricVec(const std::vector<Metric>& metvec) {
  std::string msg;

  fStatNPoint += metvec.size();
  for (auto& met : metvec) {
    fStatNTag += met.fTagset.size();
    fStatNField += met.fFieldset.size();
    msg += InfluxLine(met) + "\n"s;
    if (msg.size() > kSendChunkSize) { // limit send chunk size
      SendData(msg);
      msg.clear();
    }
  }
  if (msg.size() > 0)
    SendData(msg);
}

//-----------------------------------------------------------------------------
/*! \brief Process heartbeat
 */

void MonitorSinkInflux2::ProcessHeartbeat() {
  Monitor::Ref().QueueMetric("Monitor",                       // measurement
                             {{"host", fMonitor.HostName()}}, // no extra tags
                             {{"points", fStatNPoint},        // fields
                              {"tags", fStatNTag},
                              {"fields", fStatNField},
                              {"sends", fStatNSend},
                              {"bytes", fStatNByte},
                              {"sndtime", fStatSndTime}}); // 'time' not allowed
  fStatNPoint = 0;
  fStatNTag = 0;
  fStatNField = 0;
  fStatNSend = 0;
  fStatNByte = 0;
  fStatSndTime = 0.;
}

//-----------------------------------------------------------------------------
/*! \brief Send a set of points in line format to database
 */

void MonitorSinkInflux2::SendData(const std::string& msg) {
  try {
    // start timer
    auto tbeg = std::chrono::system_clock::now();

    // The io_context is required for all I/O
    boost::asio::io_context ioc;

    // These objects perform our I/O
    tcp::resolver resolver{ioc};
    tcp::socket socket{ioc};

    // Look up the domain name
    auto const results = resolver.resolve(fHost, fPort);

    // Make the connection on the IP address we get from a lookup
    boost::asio::connect(socket, results.begin(), results.end());

    // Set up an HTTP POST request message
    std::string target = "/api/v2/write?org=CBM&bucket="s + fBucket;
    int version = 11;
    http::request<http::string_body> req{http::verb::post, target, version};
    req.set(http::field::host, fHost);
    req.set(http::field::authorization, "Token "s + fToken);
    req.set(http::field::user_agent, "Monitor");
    req.set(http::field::accept, "application/json");
    req.set(http::field::content_type, "text/plain; charset=utf-8");
    req.set(http::field::content_length, std::to_string(msg.size()));
    req.body() = msg;

    // Send the HTTP request to the remote host
    http::write(socket, req);

    // This buffer is used for reading and must be persisted
    boost::beast::flat_buffer buffer;

    // Declare a container to hold the response
    http::response<http::string_body> res;

    // Receive the HTTP response
    http::read(socket, buffer, res);

    // Check response
    // Note on boost::beast::http::response:
    //   result() does not return the HTTP status, one gets the reason phrase as
    //   it is also returned by reason(). result_int() return the status as int.
    // Note in InfluxDB V1:
    //   returns a 204 -> "No Content" for successful completion
    //   returns a 404 -> "Not Found" if data base not existing
    //   returns a 422 -> "Unprocessable entity" if request is ill-formed
    if (res.result_int() != 200 && res.result_int() != 204) { // allow 200 & 204
      std::string efields = "";
      for (auto const& field : res) {
        efields += std::string(field.name_string());       // C++17 string_view
        efields += "=" + std::string(field.value()) + ";"; // limitation, grrr
      }
      std::string ebody = res.body(); // get body, trim \r and trailing \n
      ebody.erase(std::remove(ebody.begin(), ebody.end(), '\r'), ebody.end());
      if (!ebody.empty() && ebody[ebody.size() - 1] == '\n')
        ebody.erase(ebody.size() - 1);

#if defined(CBMLOGERR1)
      CBMLOGERR1("cid=__Monitor", "SendData-err")
          << "sinkname=" << fSinkPath << ", HTTP status=" << res.result_int()
          << " " << res.reason() << ", HTTP fields=" << efields
          << ", HTTP body=" << ebody;
#else
      std::cerr << "MonitorSinkInflux2::SendData error: "
                << "sinkname=" << fSinkPath
                << ", HTTP status=" << res.result_int() << " " << res.reason()
                << ", HTTP fields=" << efields << ", HTTP body=" << ebody
                << "\n";
#endif
    }

    // Gracefully close the socket
    boost::system::error_code ec;
    socket.shutdown(tcp::socket::shutdown_both, ec);

    // not_connected happens sometimes
    // so don't bother reporting it.
    //
    if (ec && ec != boost::system::errc::not_connected)
      throw boost::system::system_error{ec};

    // do stats
    fStatNSend += 1;
    fStatNByte += msg.size();
    std::chrono::duration<double> dt = std::chrono::system_clock::now() - tbeg;
    fStatSndTime += dt.count();

    // If we get here then the connection is closed gracefully
  } catch (std::exception const& e) {
#if defined(CBMLOGERR1)
    CBMLOGERR1("cid=__Monitor", "SendData-err")
        << "sinkname=" << fSinkPath << ", error=" << e.what();
#else
    std::cerr << "MonitorSinkInflux2::SendData error: "
              << "sinkname=" << fSinkPath << ", error=" << e.what() << "\n";
#endif
    return;
  }
  return;
}

} // end namespace cbm
