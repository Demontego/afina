#include "Connection.h"

#include <iostream>
#include <sys/socket.h>

namespace Afina {
namespace Network {
namespace MTnonblock {

// See Connection.h
void Connection::Start() {
    std::lock_guard<std::mutex> lock(mutex);
    _logger->info("Start st_nonblocking network connection on descriptor {} \n", _socket);
    _is_alive = true;
    // EPOLLIN - The associated file is available for read(2) operations.
    // EPOLLPRI - There is urgent data available for read(2) operations.
    // EPOLLRDHUP - Stream socket peer closed connection, or shut down writing half of connection.
    // EPOLLERR - Error condition happened on the associated file descriptor
    _event.events = EPOLLIN | EPOLLPRI | EPOLLRDHUP; //| EPOLLERR; - epoll_wait(2) will always wait for this event; it
                                                     // is not necessary to set it in events
    _event.data.fd = _socket;
    _event.data.ptr = this;
}

// See Connection.h
void Connection::OnError() {
    _logger->error("Error in connection on descriptor {} \n", _socket);
    OnClose();
}

// See Connection.h
void Connection::OnClose() {
    _logger->debug("Close connection of descriptor {} \n", _socket);
    _is_alive = false;
}

// See Connection.h
void Connection::DoRead() {
    std::lock_guard<std::mutex> lock(mutex);
    _logger->debug("Read from connection on descriptor {} \n", _socket);
    try {
        int readed_bytes = -1;
        char client_buffer[4096];
        while ((readed_bytes = read(_socket, client_buffer, sizeof(client_buffer))) > 0) {
            _logger->debug("Got {} bytes from socket", readed_bytes);

            // Single block of data readed from the socket could trigger inside actions a multiple times,
            // for example:
            // - read#0: [<command1 start>]
            // - read#1: [<command1 end> <argument> <command2> <argument for command 2> <command3> ... ]
            while (readed_bytes > 0) {
                _logger->debug("Process {} bytes", readed_bytes);
                // There is no command yet
                if (!command_to_execute) {
                    std::size_t parsed = 0;
                    if (parser.Parse(client_buffer, readed_bytes, parsed)) {
                        // There is no command to be launched, continue to parse input stream
                        // Here we are, current chunk finished some command, process it
                        _logger->debug("Found new command: {} in {} bytes", parser.Name(), parsed);
                        command_to_execute = parser.Build(arg_remains);
                        if (arg_remains > 0) {
                            arg_remains += 2;
                        }
                    }

                    // Parsed might fails to consume any bytes from input stream. In real life that could happens,
                    // for example, because we are working with UTF-16 chars and only 1 byte left in stream
                    if (parsed == 0) {
                        break;
                    } else {
                        std::memmove(client_buffer, client_buffer + parsed, readed_bytes - parsed);
                        readed_bytes -= parsed;
                    }
                }

                // There is command, but we still wait for argument to arrive...
                if (command_to_execute && arg_remains > 0) {
                    _logger->debug("Fill argument: {} bytes of {}", readed_bytes, arg_remains);
                    // There is some parsed command, and now we are reading argument
                    std::size_t to_read = std::min(arg_remains, std::size_t(readed_bytes));
                    argument_for_command.append(client_buffer, to_read);

                    std::memmove(client_buffer, client_buffer + to_read, readed_bytes - to_read);
                    arg_remains -= to_read;
                    readed_bytes -= to_read;
                }

                // Thre is command & argument - RUN!
                if (command_to_execute && arg_remains == 0) {
                    _logger->debug("Start command execution");

                    std::string result;
                    command_to_execute->Execute(*pStorage, argument_for_command, result);

                    // Send response
                    _results += result + "\r\n";
                    _event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP; // |EPOLLERR

                    // Prepare for the next command
                    command_to_execute.reset();
                    argument_for_command.resize(0);
                    parser.Reset();
                }
            } // while (readed_bytes)
        }
        // EAGAIN - Resource temporarily unvailable
        if (readed_bytes == 0 || errno == EAGAIN) {
            _logger->debug("Client stop to write to connection on descriptor {}", _socket);
        } else {
            throw std::runtime_error(std::string(strerror(errno)));
        }
    } catch (std::runtime_error &ex) {
        _logger->error("failed to read from connection on descriptor {}: {}", _socket, ex.what());
    }
}

// See Connection.h
void Connection::DoWrite() {
    std::lock_guard<std::mutex> lock(mutex);
    _logger->debug("Writing in connection on descriptor {} \n", _socket);
    try {
        int writed_bytes = send(_socket, _results.data(), _results.size(), 0);
        if (writed_bytes > 0) {
            if (writed_bytes < _results.size()) {
                _results.erase(0, writed_bytes);
                // EPOLLOUT - The associated file is available for write(2) operations.
                _event.events = EPOLLIN | EPOLLRDHUP | EPOLLOUT;
            } else {
                _results.clear();
                _event.events = EPOLLIN | EPOLLRDHUP;
            }
        } else {
            throw std::runtime_error("Failed to write result");
        }
    } catch (std::runtime_error &ex) {
        _logger->error("Failed to writing to connection on descriptor {}: {} \n", _socket, ex.what());
        _is_alive = false;
    }
}

} // namespace MTnonblock
} // namespace Network
} // namespace Afina
