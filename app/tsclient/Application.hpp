// Copyright 2012-2013 Jan de Cuveland <cmail@cuveland.de>
#pragma once

#include "Benchmark.hpp"
#include "Monitor.hpp"
#include "Parameters.hpp"
#include "Sink.hpp"
#include "TimesliceSource.hpp"
#include "log.hpp"
#include <chrono>
#include <memory>
#include <vector>

/// %Application base class.
class Application {
public:
  explicit Application(Parameters const& par);

  Application(const Application&) = delete;
  void operator=(const Application&) = delete;

  ~Application();

  void run();

private:
  Parameters const& par_;

  /// The application's monitoring object
  std::unique_ptr<cbm::Monitor> monitor_;

  std::unique_ptr<fles::TimesliceSource> source_;
  std::vector<std::unique_ptr<fles::TimesliceSink>> sinks_;
  std::unique_ptr<Benchmark> benchmark_;

  uint64_t count_ = 0;

  logging::OstreamLog status_log_{status};
  logging::OstreamLog debug_log_{debug};
  std::string output_prefix_;

  std::chrono::high_resolution_clock::time_point time_begin_;
  uint64_t first_ts_start_time_;

  void rate_limit_delay() const;
  void native_speed_delay(uint64_t ts_start_time);
};
