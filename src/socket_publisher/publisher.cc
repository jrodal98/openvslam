#include "socket_publisher/publisher.h"

#include "openvslam/system.h"
#include "openvslam/publish/frame_publisher.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

namespace socket_publisher
{

publisher::publisher(const std::shared_ptr<openvslam::config> &cfg, openvslam::system *system,
                     const std::shared_ptr<openvslam::publish::frame_publisher> &frame_publisher,
                     const std::shared_ptr<openvslam::publish::map_publisher> &map_publisher)
    : system_(system),
      emitting_interval_(cfg->yaml_node_["Socket.emitting_interval"].as<unsigned int>(15000)),
      image_quality_(cfg->yaml_node_["Socket.image_quality"].as<unsigned int>(20))
//   client_(new socket_client(cfg->yaml_node_["Socket.server_uri"].as<std::string>("http://127.0.0.1:3000")))
{

    const auto camera = cfg->camera_;
    const auto img_cols = (camera->cols_ < 1) ? 640 : camera->cols_;
    const auto img_rows = (camera->rows_ < 1) ? 480 : camera->rows_;
    data_serializer_ = std::unique_ptr<data_serializer>(new data_serializer(frame_publisher, map_publisher, img_cols, img_rows));

    // client_->set_signal_callback(std::bind(&publisher::callback, this, std::placeholders::_1));
}

void error(const char *msg)
{
    perror(msg);
    exit(0);
}

void write_to_rust(int sockfd, std::string map_data)
{
    uint64_t n = map_data.length();
    if (write(sockfd, &n, sizeof(uint64_t)) < 0)
        error("ERROR writing to socket");
    if (write(sockfd, map_data.c_str(), map_data.length()) < 0)
        error("ERROR writing to socket");
}

void publisher::run()
{

    struct sockaddr_in serv_addr;
    struct hostent *server;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1)
        error("ERROR opening socket");

    server = gethostbyname("127.0.0.1");
    bzero((char *)&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr,
          (char *)&serv_addr.sin_addr.s_addr,
          server->h_length);
    serv_addr.sin_port = htons(8000);
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        error("ERROR connecting");

    is_terminated_ = false;
    is_paused_ = false;

    // const auto serialized_reset_signal = data_serializer::serialized_reset_signal_;
    // client_->emit("map_publish", serialized_reset_signal);
    while (true)
    {
        const auto t0 = std::chrono::system_clock::now();

        const auto serialized_map_data = data_serializer_->serialize_map_diff();
        if (!serialized_map_data.empty())
        {
            // client_->emit("map_publish", serialized_map_data);
            write_to_rust(sockfd, serialized_map_data);
        }

        // NOT DOING ANYTHING WITH THE FRAME RIGHT NOW
        // const auto serialized_frame_data = data_serializer_->serialize_latest_frame(image_quality_);
        // if (!serialized_frame_data.empty())
        // {
        //     client_->emit("image_publish", serialized_frame_data);
        // }

        // sleep until emitting interval time is past
        const auto t1 = std::chrono::system_clock::now();
        const auto elapse_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        if (elapse_us < emitting_interval_)
        {
            const auto sleep_us = emitting_interval_ - elapse_us;
            std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
        }

        if (check_and_execute_pause())
        {
            while (is_paused())
            {
                std::this_thread::sleep_for(std::chrono::microseconds(5000));
            }
        }

        if (terminate_is_requested())
        {
            break;
        }
    }
    close(sockfd);
    system_->request_terminate();
    terminate();
}

void publisher::callback(const std::string &message)
{
    if (message == "localization_mode_true")
    {
        system_->disable_mapping_module();
    }
    else if (message == "localization_mode_false")
    {
        system_->enable_mapping_module();
    }
    else if (message == "reset")
    {
        system_->request_reset();
    }
    else if (message == "terminate")
    {
        request_terminate();
    }
}

void publisher::request_pause()
{
    std::lock_guard<std::mutex> lock(mtx_pause_);
    if (!is_paused_)
    {
        pause_is_requested_ = true;
    }
}

bool publisher::is_paused()
{
    std::lock_guard<std::mutex> lock(mtx_pause_);
    return is_paused_;
}

void publisher::resume()
{
    std::lock_guard<std::mutex> lock(mtx_pause_);
    is_paused_ = false;
}

void publisher::request_terminate()
{
    std::lock_guard<std::mutex> lock(mtx_terminate_);
    terminate_is_requested_ = true;
}

bool publisher::is_terminated()
{
    std::lock_guard<std::mutex> lock(mtx_terminate_);
    return is_terminated_;
}

bool publisher::terminate_is_requested()
{
    std::lock_guard<std::mutex> lock(mtx_terminate_);
    return terminate_is_requested_;
}

void publisher::terminate()
{
    std::lock_guard<std::mutex> lock(mtx_terminate_);
    is_terminated_ = true;
}

bool publisher::check_and_execute_pause()
{
    std::lock_guard<std::mutex> lock1(mtx_pause_);
    std::lock_guard<std::mutex> lock2(mtx_terminate_);

    if (terminate_is_requested_)
    {
        return false;
    }
    else if (pause_is_requested_)
    {
        is_paused_ = true;
        pause_is_requested_ = false;
        return true;
    }
    return false;
}

} // namespace socket_publisher
