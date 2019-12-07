#include "Connection.h"

#include <iostream>
#include <sys/socket.h>
#include <sys/uio.h>
namespace Afina {
namespace Network {
namespace STnonblock {

// See Connection.h
void Connection::Start() {
    _logger->info("Start st_nonblocking network connection on descriptor {} \n", _socket);
    _is_alive = true;
    // EPOLLIN - The associated file is available for read(2) operations.
    // EPOLLPRI - There is urgent data available for read(2) operations.
    // EPOLLRDHUP - Stream socket peer closed connection, or shut down writing half of connection.
    // EPOLLERR - Error condition happened on the associated file descriptor
    _event.events = EPOLLIN | EPOLLPRI | EPOLLRDHUP; //| EPOLLERR; - epoll_wait(2) will always wait for this event; it
                                                     // is not necessary to set it in events

    command_to_execute.reset();
    argument_for_command.resize(0);
    parser.Reset();
    arg_remains = 0;

    // Prepare for reading
    _written_bytes = 0;
    _read_bytes = 0;
    _results.clear();
    _event.data.ptr = this;
    /* typedef union epoll_data {
    void    *ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;
*/
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
    _logger->debug("Read from connection on descriptor {} \n", _socket);
    int client_socket = _socket;
    command_to_execute = nullptr;
    try {
        int _bytes_for_read;
        while ((_bytes_for_read =
                    read(client_socket, client_buffer + _read_bytes, sizeof(client_buffer) - _read_bytes)) > 0) {
            //_logger->debug("Got {} bytes from socket", _read_bytes);
            _read_bytes += _bytes_for_read;
            // Single block of data readed from the socket could trigger inside actions a multiple times,
            // for example:
            // - read#0: [<command1 start>]
            // - read#1: [<command1 end> <argument> <command2> <argument for command 2> <command3> ... ]
            while (_read_bytes > 0) {
                _logger->debug("Process {} bytes", _read_bytes);
                // There is no command yet
                if (!command_to_execute) {
                    std::size_t parsed = 0;
                    if (parser.Parse(client_buffer, _read_bytes, parsed)) {
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
                        std::memmove(client_buffer, client_buffer + parsed, _read_bytes - parsed);
                        _read_bytes -= parsed;
                    }
                }

                // There is command, but we still wait for argument to arrive...
                if (command_to_execute && arg_remains > 0) {
                    _logger->debug("Fill argument: {} bytes of {}", _read_bytes, arg_remains);
                    // There is some parsed command, and now we are reading argument
                    std::size_t to_read = std::min(arg_remains, std::size_t(_read_bytes));
                    argument_for_command.append(client_buffer, to_read);
                    std::memmove(client_buffer, client_buffer + to_read, _read_bytes - to_read);
                    arg_remains -= to_read;
                    _read_bytes -= to_read;
                }

                // Thre is command & argument - RUN!
                if (command_to_execute && arg_remains == 0) {
                    _logger->debug("Start command execution");

                    std::string result;
                    command_to_execute->Execute(*pStorage, argument_for_command, result);

                    // Send response
                    result += "\r\n";
                    _results.push_back(result);
                    if (_results.empty()) {
                        _event.events = (EPOLLIN | EPOLLOUT | EPOLLRDHUP);
                    };

                    // Prepare for the next command
                    command_to_execute.reset();
                    argument_for_command.resize(0);
                    parser.Reset();
                }
            } // while (readed_bytes)
        }
        // EAGAIN - Resource temporarily unvailable
        _logger->debug("Client stop to write to connection on descriptor {}", client_socket);
        if (_read_bytes > 0 && errno != EAGAIN) {
            throw std::runtime_error(std::string(strerror(errno)));
        } else {
        }
    } catch (std::runtime_error &ex) {
        _logger->error("failed to read from connection on descriptor {}: {}", client_socket, ex.what());
    }
}

// See Connection.h
void Connection::DoWrite() {
    _logger->debug("Writing in connection on descriptor {} \n", _socket);
    try {
        std::size_t size = _results.size();
        auto it = _results.begin();
        struct iovec iov[size];
        /*
        struct iovec {
        void  *iov_base;
        size_t iov_len; };
        */
        for (std::size_t i = 0; i < size; ++i, ++it) {
            iov[i].iov_base = &(*it)[0]; // begin of string, which in vector;
            iov[i].iov_len = (*it).size();
        }
        iov[0].iov_base = (char *)iov[0].iov_base + _written_bytes;
        iov[0].iov_len -= _written_bytes;

        int written = writev(_socket, iov, size); //Системный вызов writev() записывает iovcnt буферов, описанных iov,
        _written_bytes += written; // в файл, связанный с файловым дескриптором fd («сборный вывод»).
        it = _results.begin();
        for (auto del_it = &iov[0]; (*del_it).iov_len < _written_bytes; ++del_it, ++it) {
            _written_bytes -= (*del_it).iov_len;
        }

        _results.erase(_results.begin(), it);
        if (_results.empty()) {
            _event.events = (EPOLLIN | EPOLLRDHUP);
        }
    } catch (std::runtime_error &ex) {
        _logger->error("Failed to writing to connection on descriptor {}: {} \n", _socket, ex.what());
        _is_alive = false;
    }
}

} // namespace STnonblock
} // namespace Network
} // namespace Afina
