/* Copyright 2016-2019 Fizyr B.V. - https://fizyr.com
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "../connect.hpp"
#include "./read_file.hpp"
#include "./write_file.hpp"

#include "commands.hpp"
#include "udp/client.hpp"
#include "udp/message.hpp"
#include "udp/protocol.hpp"

#include <atomic>
#include <memory>
#include <utility>

namespace dr {
namespace yaskawa {
namespace udp {

Client::Client(asio::io_service & ios) :
	socket_(ios),
	read_buffer_{std::make_unique<std::array<std::uint8_t, 512>>()} {}

void Client::connect(std::string const & host, std::string const & port, std::chrono::milliseconds timeout, ErrorCallback callback) {
	auto on_connect = [this, callback = std::move(callback)] (Error error) {
		onConnect(error, std::move(callback));
	};
	asyncResolveConnect({host, port}, timeout, socket_, on_connect);
}

void Client::connect(std::string const & host, std::uint16_t port, std::chrono::milliseconds timeout, ErrorCallback callback) {
	connect(host, std::to_string(port), timeout, callback);
}

void Client::close() {
	socket_.close();
}

Client::HandlerToken Client::registerHandler(std::uint8_t request_id, std::function<void(ResponseHeader const &, std::string_view)> handler) {
	auto result = requests_.insert({request_id, {std::chrono::steady_clock::now(), handler}});
	if (!result.second) throw std::logic_error("request_id " + std::to_string(request_id) + " is already taken, can not register handler");
	return result.first;
}

void Client::removeHandler(HandlerToken token) {
	requests_.erase(token);
}

// File control.

void Client::readFileList(
	std::string type,
	std::chrono::milliseconds timeout,
	std::function<void(Result<std::vector<std::string>>)> on_done,
	std::function<void(std::size_t bytes_received)> on_progress
) {
	impl::readFile(*this, request_id_++, ReadFileList{std::move(type)}, timeout, std::move(on_done), std::move(on_progress));
}

void Client::readFile(
	std::string name,
	std::chrono::milliseconds timeout,
	std::function<void(Result<std::string>)> on_done,
	std::function<void(std::size_t bytes_received)> on_progress
) {
	impl::readFile(*this, request_id_++, ReadFile{std::move(name)}, timeout, std::move(on_done), std::move(on_progress));
}

void Client::writeFile(
	std::string name,
	std::string data,
	std::chrono::milliseconds timeout,
	std::function<void(Result<void>)> on_done,
	std::function<void(std::size_t bytes_sent, std::size_t total_bytes)> on_progress
) {
	impl::writeFile(*this, request_id_++, WriteFile{std::move(name), std::move(data)}, timeout, std::move(on_done), std::move(on_progress));
}

void Client::deleteFile(
	std::string name,
	std::chrono::milliseconds timeout,
	std::function<void(Result<void>)> on_done
) {
	sendCommand(DeleteFile{std::move(name)}, timeout, on_done);
}

// Other stuff

void Client::onConnect(Error error, ErrorCallback callback) {
	callback(error);
	if (!error) receive();
}

void Client::receive() {
	// Make sure we stop reading if the socket is closed.
	// Otherwise in rare cases we can miss an operation_canceled and continue reading forever.
	if (!socket_.is_open()) return;
	auto callback = std::bind(&Client::onReceive, this, std::placeholders::_1, std::placeholders::_2);
	socket_.async_receive(asio::buffer(read_buffer_->data(), read_buffer_->size()), callback);
}

void Client::onReceive(std::error_code error, std::size_t message_size) {
	if (error == std::errc::operation_canceled) return;
	if (error) {
		if (on_error) on_error(make_error_code(std::errc(error.value())));
		receive();
		return;
	}

	// Decode the response header.
	std::string_view message{reinterpret_cast<char const *>(read_buffer_->data()), message_size};
	Result<ResponseHeader> header = decodeResponseHeader(message);
	if (!header) {
		if (on_error) on_error(header.error());
		receive();
		return;
	}

	// Find the right handler for the response.
	auto handler = requests_.find(header->request_id);
	if (handler == requests_.end()) {
		if (on_error) on_error({errc::unknown_request, "no handler for request id " + std::to_string(header->request_id)});
		receive();
		return;
	}

	// Invoke the handler (a copy, so it can erase itself safely).
	auto callback = handler->second.on_reply;
	callback(*header, message);
	receive();
}

}}}
