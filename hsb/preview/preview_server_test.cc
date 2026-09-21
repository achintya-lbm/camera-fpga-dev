// Drives PreviewServer over a real TCP socket on an ephemeral port with a fake sink and controls.
#include "hsb/preview/preview_server.hpp"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace hsb::preview {
namespace {

class FakeControls : public Controls {
 public:
  ControlState Get() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
  }
  std::string Apply(const std::map<std::string, std::string>& values) override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [k, v] : values) {
      if (k == "exposure_ms") {
        const double x = std::stod(v);
        if (x < 0) return "exposure_ms must be >= 0";
        state_.exposure_ms = x;
      } else if (k == "gain_db") {
        state_.gain_db = std::stod(v);
      } else if (k == "test_pattern") {
        state_.test_pattern = v == "1" || v == "true";
      }
    }
    applied++;
    return "";
  }
  std::string CaptureRaw() override { return "captures/stills/fake_20260921-120000-000"; }
  std::string StatusJson() override { return "{\"fps\": 32.65, \"gbps\": 4.12, \"frames\": 100}"; }

  std::mutex mutex_;
  ControlState state_{"cam3", "FULL_RAW10", 3552, 3556, 32.65, 5.0, 3.0, 50, false, 0, 30.6, 72.0};
  std::atomic<int> applied{0};
};

std::vector<uint8_t> FakeJpeg(size_t payload) {
  std::vector<uint8_t> j(payload + 4, 0x42);
  j[0] = 0xFF;
  j[1] = 0xD8;
  j[j.size() - 2] = 0xFF;
  j[j.size() - 1] = 0xD9;
  return j;
}

// Sends one HTTP request and reads until the peer closes or `max_bytes` arrived.
std::string Http(uint16_t port, const std::string& request, size_t max_bytes = 1 << 20, int timeout_ms = 3000) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return "";
  }
  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
  std::string out;
  char buf[8192];
  while (out.size() < max_bytes) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  ::close(fd);
  return out;
}

std::string Get(uint16_t port, const std::string& path, size_t max_bytes = 1 << 20, int timeout_ms = 3000) {
  return Http(port, "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n", max_bytes, timeout_ms);
}

std::string Post(uint16_t port, const std::string& path, const std::string& body, const std::string& content_type) {
  return Http(port, "POST " + path + " HTTP/1.1\r\nHost: localhost\r\nContent-Type: " + content_type +
                        "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
}

std::string Body(const std::string& response) {
  const auto pos = response.find("\r\n\r\n");
  return pos == std::string::npos ? "" : response.substr(pos + 4);
}

std::string StatusLine(const std::string& response) { return response.substr(0, response.find("\r\n")); }

class PreviewServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    still_dir_ = std::filesystem::temp_directory_path() / ("preview_server_test_" + std::to_string(::getpid()));
    std::filesystem::create_directories(still_dir_);
    sink_ = std::make_shared<PreviewSink>();
    controls_ = std::make_shared<FakeControls>();
    PreviewServerOptions options;
    options.bind_address = "127.0.0.1";
    options.port = 0;  // ephemeral
    options.title = "test";
    options.still_dir = still_dir_.string();
    options.stream_fps = 100;
    server_ = std::make_unique<PreviewServer>(options, sink_, controls_);
    server_->Start();
    ASSERT_NE(server_->port(), 0);
  }
  void TearDown() override {
    server_->Stop();
    std::filesystem::remove_all(still_dir_);
  }

  std::filesystem::path still_dir_;
  std::shared_ptr<PreviewSink> sink_;
  std::shared_ptr<FakeControls> controls_;
  std::unique_ptr<PreviewServer> server_;
};

TEST_F(PreviewServerTest, ServesPage) {
  const auto r = Get(server_->port(), "/");
  EXPECT_EQ(StatusLine(r), "HTTP/1.1 200 OK");
  EXPECT_NE(r.find("text/html"), std::string::npos);
  EXPECT_NE(Body(r).find("/stream.mjpg"), std::string::npos);
  EXPECT_NE(Body(r).find("<title>test</title>"), std::string::npos);
}

TEST_F(PreviewServerTest, StatusMergesControlStateAndStatusJson) {
  sink_->Publish({FakeJpeg(10), 0, 8, 8, {}});
  const auto r = Get(server_->port(), "/status.json");
  EXPECT_EQ(StatusLine(r), "HTTP/1.1 200 OK");
  const auto body = Body(r);
  EXPECT_NE(body.find("\"camera\":\"cam3\""), std::string::npos) << body;
  EXPECT_NE(body.find("\"mode\":\"FULL_RAW10\""), std::string::npos) << body;
  EXPECT_NE(body.find("\"width\":3552"), std::string::npos) << body;
  EXPECT_NE(body.find("\"exposure_ms\":5.0000"), std::string::npos) << body;
  EXPECT_NE(body.find("\"gbps\": 4.12"), std::string::npos) << body;
  EXPECT_NE(body.find("\"stream_frames\":1"), std::string::npos) << body;
  EXPECT_EQ(body.front(), '{');
  EXPECT_EQ(body.back(), '}');
}

TEST_F(PreviewServerTest, SnapshotReturnsLatestJpeg) {
  EXPECT_EQ(StatusLine(Get(server_->port(), "/snapshot.jpg", 1 << 20, 3000)), "HTTP/1.1 504 Gateway Timeout");
  const auto jpeg = FakeJpeg(1000);
  sink_->Publish({jpeg, 0, 16, 16, {}});
  const auto r = Get(server_->port(), "/snapshot.jpg");
  EXPECT_EQ(StatusLine(r), "HTTP/1.1 200 OK");
  EXPECT_NE(r.find("Content-Type: image/jpeg"), std::string::npos);
  EXPECT_EQ(Body(r), std::string(jpeg.begin(), jpeg.end()));
}

TEST_F(PreviewServerTest, ControlAcceptsFormAndJson) {
  auto r = Post(server_->port(), "/control", "exposure_ms=12.5&gain_db=6", "application/x-www-form-urlencoded");
  EXPECT_EQ(StatusLine(r), "HTTP/1.1 200 OK") << r;
  EXPECT_EQ(Body(r), "{\"ok\":true}");
  EXPECT_DOUBLE_EQ(controls_->Get().exposure_ms, 12.5);
  EXPECT_DOUBLE_EQ(controls_->Get().gain_db, 6.0);

  r = Post(server_->port(), "/control", "{\"exposure_ms\": 3.25, \"test_pattern\": \"1\"}", "application/json");
  EXPECT_EQ(StatusLine(r), "HTTP/1.1 200 OK") << r;
  EXPECT_DOUBLE_EQ(controls_->Get().exposure_ms, 3.25);
  EXPECT_TRUE(controls_->Get().test_pattern);

  r = Post(server_->port(), "/control", "exposure_ms=-1", "application/x-www-form-urlencoded");
  EXPECT_EQ(StatusLine(r), "HTTP/1.1 400 Bad Request") << r;
  EXPECT_NE(Body(r).find("exposure_ms must be"), std::string::npos);

  r = Post(server_->port(), "/control", "", "application/x-www-form-urlencoded");
  EXPECT_EQ(StatusLine(r), "HTTP/1.1 400 Bad Request") << r;
  EXPECT_EQ(controls_->applied.load(), 2);  // the rejected value never reaches the counter
}

TEST_F(PreviewServerTest, CaptureReturnsPath) {
  const auto r = Post(server_->port(), "/capture", "", "application/json");
  EXPECT_EQ(StatusLine(r), "HTTP/1.1 200 OK") << r;
  EXPECT_EQ(Body(r), "{\"ok\":true,\"path\":\"captures/stills/fake_20260921-120000-000\"}");
}

TEST_F(PreviewServerTest, StreamDeliversMultipartFrames) {
  std::atomic<bool> stop{false};
  std::thread producer([&] {
    uint8_t marker = 0;
    while (!stop) {
      sink_->Publish({FakeJpeg(100 + marker), 0, 8, 8, {}});
      marker = static_cast<uint8_t>((marker + 1) % 100);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  // Read a bounded amount: headers + at least two parts.
  const auto r = Get(server_->port(), "/stream.mjpg", 600, 3000);
  stop = true;
  producer.join();
  EXPECT_EQ(StatusLine(r), "HTTP/1.1 200 OK") << r.substr(0, 200);
  EXPECT_NE(r.find("multipart/x-mixed-replace; boundary=frame"), std::string::npos);
  const auto first = r.find("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ");
  ASSERT_NE(first, std::string::npos);
  EXPECT_NE(r.find("--frame\r\n", first + 8), std::string::npos) << "expected a second part";
  // The JPEG bytes follow the part header.
  const auto data = r.find("\r\n\r\n", first) + 4;
  EXPECT_EQ(static_cast<uint8_t>(r[data]), 0xFF);
  EXPECT_EQ(static_cast<uint8_t>(r[data + 1]), 0xD8);
}

TEST_F(PreviewServerTest, StillIsSavedAndListed) {
  std::thread encoder([&] {
    // Emulate JpegEncoderOp: wait for the request, publish a "full-res" still.
    for (int i = 0; i < 400; ++i) {
      if (sink_->TakeStillRequest()) {
        sink_->PublishStill({FakeJpeg(5000), 0, 3552, 3556, {}});
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  const auto r = Get(server_->port(), "/still.jpg");
  encoder.join();
  EXPECT_EQ(StatusLine(r), "HTTP/1.1 200 OK") << r.substr(0, 200);
  EXPECT_EQ(Body(r).size(), 5004u);
  const auto x = r.find("X-File: cam3_");
  ASSERT_NE(x, std::string::npos);
  const std::string name = r.substr(x + 8, r.find("\r\n", x) - x - 8);
  EXPECT_TRUE(std::filesystem::exists(still_dir_ / name)) << name;
  EXPECT_EQ(std::filesystem::file_size(still_dir_ / name), 5004u);

  const auto list = Body(Get(server_->port(), "/files.json"));
  EXPECT_NE(list.find("\"name\":\"" + name + "\""), std::string::npos) << list;
  const auto mpos = list.find("\"mtime\":");
  ASSERT_NE(mpos, std::string::npos) << list;
  EXPECT_GT(std::stoll(list.substr(mpos + 8)), 1'700'000'000LL) << list;  // Unix seconds, not file_clock ticks
  const auto file = Get(server_->port(), "/files/" + name);
  EXPECT_EQ(StatusLine(file), "HTTP/1.1 200 OK");
  EXPECT_EQ(Body(file).size(), 5004u);
  EXPECT_EQ(StatusLine(Get(server_->port(), "/files/../preview_server_test.cc")), "HTTP/1.1 400 Bad Request");
  EXPECT_EQ(StatusLine(Get(server_->port(), "/files/nope.jpg")), "HTTP/1.1 404 Not Found");
}

TEST_F(PreviewServerTest, StillTimesOutWithoutEncoder) {
  const auto start = std::chrono::steady_clock::now();
  const auto r = Get(server_->port(), "/still.jpg", 1 << 20, 8000);
  EXPECT_EQ(StatusLine(r), "HTTP/1.1 504 Gateway Timeout") << r.substr(0, 200);
  EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(4900));
}

TEST_F(PreviewServerTest, UnknownRouteIs404) {
  EXPECT_EQ(StatusLine(Get(server_->port(), "/nope")), "HTTP/1.1 404 Not Found");
}

}  // namespace
}  // namespace hsb::preview
