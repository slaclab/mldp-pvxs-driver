#include <controller/MLDPPVXSController.h>

#include <spdlog/spdlog.h>

#include <chrono>
#include <format>
#include <grpcpp/grpcpp.h>
#include <string>

using namespace mldp_pvxs_driver::controller;
using mldp_pvxs_driver::util::pool::MLDPGrpcObject;
using mldp_pvxs_driver::util::pool::MLDPGrpcPool;

MLDPPVXSController::MLDPPVXSController(const config::Config& config)
    : config_(config)
    , thread_pool_(std::make_shared<BS::light_thread_pool>(config_.controllerThreadPoolSize()))
    , mldp_pool_(MLDPGrpcPool::create(
          static_cast<std::size_t>(config_.pool().min_conn),
          static_cast<std::size_t>(config_.pool().max_conn),
          [endpoint = config_.pool().url]()
          {
              auto channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
              return std::make_shared<MLDPGrpcObject>(std::move(channel));
          }))
    , running_(false)
{
    // Constructor implementation
}

MLDPPVXSController::~MLDPPVXSController()
{
    // Destructor implementation
}

void MLDPPVXSController::start()
{
    running_ = true;
    // Start controller logic
}

void MLDPPVXSController::stop()
{
    running_ = false;
    // Stop controller logic
}

bool MLDPPVXSController::push(EventValue data_value)
{
    // Stub implementation until controller wires into a real bus.
    thread_pool_->detach_task([this, data_value]()
                              {
                                  auto millisecond = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch());
                                  dp::service::ingestion::IngestDataRequest request;
                                  request.set_providerid(config_.pool().provider_name);
                                  request.set_clientrequestid(std::format("pv_{}_{}", data_value->src_name, millisecond.count()));
                                  request.add_tags(data_value->src_name);

                                  auto* dataFrame = request.mutable_ingestiondataframe();
                                  auto* timestamps = dataFrame->mutable_datatimestamps();
                                  auto* timestampList = timestamps->mutable_timestamplist();
                                  auto* ts = timestampList->add_timestamps();
                                  if (data_value->epoch_seconds)
                                  {
                                      // Use provided timestamp
                                      ts->set_epochseconds(data_value->epoch_seconds);
                                      // Use provided nanoseconds if any
                                      if (data_value->nanoseconds)
                                      {
                                          ts->set_nanoseconds(data_value->nanoseconds);
                                      }
                                  }
                                  else
                                  {
                                      // Fallback to make sure timestamp is always set
                                      const auto now = std::chrono::system_clock::now().time_since_epoch();
                                      ts->set_epochseconds(std::chrono::duration_cast<std::chrono::seconds>(now).count());
                                  }

                                  auto* column = dataFrame->add_datacolumns();
                                  column->set_name(data_value->src_name);

                                  auto* dataValue = column->add_datavalues();
                                  *dataValue = std::move(*data_value->data_value);

                                  {
                                      grpc::ClientContext                        context;
                                      dp::service::ingestion::IngestDataResponse response;
                                      auto                                       pool_instance = mldp_pool_->acquire();
                                      if (const auto status = pool_instance->stub->ingestData(&context, request, &response); !status.ok())
                                      {
                                          spdlog::error("Ingestion failed for {}: {}", data_value->src_name, status.error_message());
                                      }
                                  }
                              });
    return true;
}