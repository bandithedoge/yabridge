// yabridge: a Wine VST bridge
// Copyright (C) 2020  Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "group.h"

#include <unistd.h>
#include <boost/asio/read_until.hpp>
#include <boost/process/environment.hpp>
#include <regex>

#include "../../common/communication.h"

// FIXME: `std::filesystem` is broken in wineg++, at least under Wine 5.8. Any
//        path operation will thrown an encoding related error
namespace fs = boost::filesystem;

/**
 * Listen on the specified endpoint if no process is already listening there,
 * otherwise throw. This is needed to handle these three situations:
 *
 * 1. The endpoint does not already exist, and we can simply create an endpoint.
 * 2. The endpoint already exists but it is stale and no process is currently
 *    listening. In this case we can remove the file and start listening.
 * 3. The endpoint already exists and another process is currently listening on
 *    it. In this situation we will throw immediately and we'll terminate this
 *    process.
 *
 * If anyone knows a better way to handle this, please let me know!
 *
 * @throw std::runtime_error If another process is already listening on the
 *        endpoint.
 */
boost::asio::local::stream_protocol::acceptor create_acceptor_if_inactive(
    boost::asio::io_context& io_context,
    boost::asio::local::stream_protocol::endpoint& endpoint);

/**
 * Create a logger prefix containing the group name based on the socket path.
 */
std::string create_logger_prefix(const fs::path& socket_path);

uint32_t WINAPI handle_host_plugin_proxy(void* param);

/**
 * CreateThread() is great and allows you to pass a single value to the
 * function, so we'll use this to pass both `this` and the parameters to the
 * below thread function so it can do its thing.
 *
 * @relates handle_host_plugin_proxy
 */
using handle_host_plugin_parameters = std::pair<GroupBridge*, GroupRequest>;

StdIoCapture::StdIoCapture(boost::asio::io_context& io_context,
                           int file_descriptor)
    : pipe(io_context),
      target_fd(file_descriptor),
      original_fd_copy(dup(file_descriptor)) {
    // We'll use the second element of these two file descriptors to reopen
    // `file_descriptor`, and the first one to read the captured contents from
    ::pipe(pipe_fd);

    // We've already created a copy of the original file descriptor, so we can
    // reopen it using the newly created pipe
    dup2(pipe_fd[1], target_fd);
    close(pipe_fd[1]);

    pipe.assign(pipe_fd[0]);
}

StdIoCapture::~StdIoCapture() {
    // Restore the original file descriptor and close the pipe. The other wend
    // was already closed in the constructor.
    dup2(original_fd_copy, target_fd);
    close(original_fd_copy);
    close(pipe_fd[0]);
}

GroupBridge::GroupBridge(boost::filesystem::path group_socket_path)
    : logger(Logger::create_from_environment(
          create_logger_prefix(group_socket_path))),
      io_context(),
      stdio_context(),
      stdout_redirect(stdio_context, STDOUT_FILENO),
      stderr_redirect(stdio_context, STDERR_FILENO),
      group_socket_endpoint(group_socket_path.string()),
      group_socket_acceptor(
          create_acceptor_if_inactive(io_context, group_socket_endpoint)),
      shutdown_timer(io_context) {
    // Write this process's original STDOUT and STDERR streams to the logger
    async_log_pipe_lines(stdout_redirect.pipe, stdout_buffer, "[STDOUT] ");
    async_log_pipe_lines(stderr_redirect.pipe, stderr_buffer, "[STDERR] ");

    stdio_handler = std::thread([&]() { stdio_context.run(); });
}

GroupBridge::~GroupBridge() {
    stdio_context.stop();
    stdio_handler.join();
}

void GroupBridge::handle_host_plugin(const GroupRequest request) {
    using namespace std::literals::chrono_literals;

    // At this point the `active_plugins` map will already contain a copy of
    // `parameters` along with this thread's handle
    // The initialization process for a plugin is identical to that in
    // `../individual-host.cpp`
    logger.log("Received request to host '" + request.plugin_path +
               "' using socket '" + request.socket_path + "'");
    try {
        Vst2Bridge bridge(request.plugin_path, request.socket_path);
        logger.log("Finished initializing '" + request.plugin_path + "'");

        // Blocks the main thread until the plugin shuts down
        bridge.handle_dispatch();

        logger.log("'" + request.plugin_path + "' has exited");
    } catch (const std::runtime_error& error) {
        logger.log("Error while initializing '" + request.plugin_path + "':");
        logger.log(error.what());
    }

    // After the plugin has exited (either after `effClose()` or because it
    // failed to initialize), we'll remove this thread's plugin from the active
    // plugins. If no active plugins remain, then we'll terminate this process.
    std::lock_guard lock(active_plugins_mutex);
    active_plugins.erase(request);

    // Defer shutting down the process to allow for fast plugin scanning by
    // allowing plugins to reuse the same group host process
    shutdown_timer.expires_after(2s);
    shutdown_timer.async_wait([&](const boost::system::error_code& error) {
        // A previous timer gets canceled automatically when another plugin
        // exits
        if (error.failed()) {
            return;
        }

        std::lock_guard lock(active_plugins_mutex);
        if (active_plugins.size() == 0) {
            logger.log(
                "All plugins have exited, shutting down the group process");
            io_context.stop();
        }
    });
}

void GroupBridge::handle_incoming_connections() {
    accept_requests();

    logger.log(
        "Group host is up and running, now accepting incoming connections");
    io_context.run();
}

void GroupBridge::accept_requests() {
    group_socket_acceptor.async_accept(
        [&](const boost::system::error_code& error,
            boost::asio::local::stream_protocol::socket socket) {
            std::lock_guard lock(active_plugins_mutex);

            // Stop the whole process when the socket gets closed unexpectedly
            if (error.failed()) {
                logger.log("Error while listening for incoming connections:");
                logger.log(error.message());

                io_context.stop();
            }

            // Read the parameters, and then host the plugin in this process
            // just like if we would be hosting the plugin individually through
            // `yabridge-hsot.exe`. We will reply with this process's PID so the
            // yabridge plugin will be able to tell if the plugin has caused
            // this process to crash during its initialization to prevent
            // waiting indefinitely on the sockets to be connected to.
            const auto parameters = read_object<GroupRequest>(socket);
            write_object(socket, GroupResponse{boost::this_process::get_id()});

            // Collisions in the generated socket names should be very rare, but
            // it could in theory happen
            assert(active_plugins.find(parameters) == active_plugins.end());

            // CreateThread() doesn't support multiple arguments and requires
            // manualy memory management.
            handle_host_plugin_parameters* thread_params =
                new std::pair<GroupBridge*, GroupRequest>(this, parameters);
            active_plugins[parameters] =
                Win32Thread(handle_host_plugin_proxy, thread_params);

            accept_requests();
        });
}

void GroupBridge::async_log_pipe_lines(
    boost::asio::posix::stream_descriptor& pipe,
    boost::asio::streambuf& buffer,
    std::string prefix) {
    boost::asio::async_read_until(
        pipe, buffer, '\n',
        [&, prefix](const boost::system::error_code& error, size_t) {
            // When we get an error code then that likely means that the pipe
            // has been clsoed and we have reached the end of the file
            if (error.failed()) {
                return;
            }

            std::string line;
            std::getline(std::istream(&buffer), line);
            logger.log(prefix + line);

            async_log_pipe_lines(pipe, buffer, prefix);
        });
}

boost::asio::local::stream_protocol::acceptor create_acceptor_if_inactive(
    boost::asio::io_context& io_context,
    boost::asio::local::stream_protocol::endpoint& endpoint) {
    // First try to listen on the endpoint normally
    try {
        return boost::asio::local::stream_protocol::acceptor(io_context,
                                                             endpoint);
    } catch (const boost::system::system_error& error) {
        // If this failed, then either there is a stale socket file or another
        // process is already is already listening. In the last case we will
        // simply throw so the other process can handle the request.
        std::ifstream open_sockets("/proc/net/unix");
        std::string endpoint_path = endpoint.path();
        for (std::string line; std::getline(open_sockets, line);) {
            if (line.size() < endpoint_path.size()) {
                continue;
            }

            std::string file = line.substr(line.size() - endpoint_path.size());
            if (file == endpoint_path) {
                // Another process is already listening, so we don't have to do
                // anything
                throw error;
            }
        }

        // At this point we can remove the stale socket and start listening
        fs::remove(endpoint_path);
        return boost::asio::local::stream_protocol::acceptor(io_context,
                                                             endpoint);
    }
}

std::string create_logger_prefix(const fs::path& socket_path) {
    // The group socket filename will be in the format
    // '/tmp/yabridge-group-<group_name>-<wine_prefix_id>-<architecture>.sock',
    // where Wine prefix ID is just Wine prefix ran through `std::hash` to
    // prevent collisions without needing complicated filenames. We want to
    // extract the group name.
    std::string socket_name =
        socket_path.filename().replace_extension().string();

    std::smatch group_match;
    std::regex group_regexp("^yabridge-group-(.*)-[^-]+-[^-]+$",
                            std::regex::ECMAScript);
    if (std::regex_match(socket_name, group_match, group_regexp)) {
        socket_name = group_match[1].str();

#ifdef __i386__
        // Mark 32-bit versions to avoid potential confusion caused by 32-bit
        // and regular 64-bit group processes with the same name running
        // alongside eachother
        socket_name += "-x32";
#endif
    }

    return "[" + socket_name + "] ";
}

uint32_t WINAPI handle_host_plugin_proxy(void* param) {
    // The Win32 API only allows you to pass a void pointer to threads, so we
    // need to use manual memory management.
    auto thread_params = static_cast<handle_host_plugin_parameters*>(param);
    GroupBridge* instance = thread_params->first;
    GroupRequest parameters = thread_params->second;
    delete thread_params;

    instance->handle_host_plugin(parameters);

    return 0;
}
