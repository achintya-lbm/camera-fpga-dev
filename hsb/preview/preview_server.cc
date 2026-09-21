#include "hsb/preview/preview_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include <fmt/format.h>
#include <holoscan/holoscan.hpp>

#include "hsb/preview/preview_page.hpp"

namespace hsb::preview {
namespace {

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) out += fmt::format("\\u{:04x}", c);
        else out += static_cast<char>(c);
    }
  }
  return out;
}

std::string UrlDecode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') {
      out += ' ';
    } else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
               std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::string Trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::map<std::string, std::string> ParseForm(const std::string& body) {
  std::map<std::string, std::string> kv;
  std::stringstream ss(body);
  std::string pair;
  while (std::getline(ss, pair, '&')) {
    const auto eq = pair.find('=');
    if (eq == std::string::npos) continue;
    kv[UrlDecode(pair.substr(0, eq))] = UrlDecode(pair.substr(eq + 1));
  }
  return kv;
}

// Flat JSON object of strings / numbers / booleans -> string map (good enough for the control page).
std::map<std::string, std::string> ParseFlatJson(const std::string& body) {
  std::map<std::string, std::string> kv;
  size_t i = body.find('{');
  if (i == std::string::npos) return kv;
  ++i;
  auto skip_ws = [&] { while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i; };
  auto read_string = [&]() -> std::string {
    std::string out;
    ++i;  // opening quote
    while (i < body.size() && body[i] != '"') {
      if (body[i] == '\\' && i + 1 < body.size()) {
        ++i;
        switch (body[i]) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          default: out += body[i];
        }
      } else {
        out += body[i];
      }
      ++i;
    }
    ++i;  // closing quote
    return out;
  };
  while (i < body.size()) {
    skip_ws();
    if (i >= body.size() || body[i] == '}') break;
    if (body[i] == ',') { ++i; continue; }
    if (body[i] != '"') break;
    const std::string key = read_string();
    skip_ws();
    if (i >= body.size() || body[i] != ':') break;
    ++i;
    skip_ws();
    std::string value;
    if (i < body.size() && body[i] == '"') {
      value = read_string();
    } else {
      const size_t start = i;
      while (i < body.size() && body[i] != ',' && body[i] != '}') ++i;
      value = Trim(body.substr(start, i - start));
    }
    kv[key] = value;
  }
  return kv;
}

std::string ContentTypeFor(const std::string& name) {
  const auto dot = name.rfind('.');
  const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "png") return "image/png";
  if (ext == "json") return "application/json";
  if (ext == "html") return "text/html; charset=utf-8";
  return "application/octet-stream";
}

std::string Timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
  return fmt::format("{}-{:03d}", buf, ms);
}

bool SendAll(int fd, const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  while (size > 0) {
    const ssize_t n = ::send(fd, p, size, MSG_NOSIGNAL);
    if (n <= 0) return false;
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

bool SendAll(int fd, const std::string& s) { return SendAll(fd, s.data(), s.size()); }

struct Request {
  std::string method;
  std::string path;
  std::string query;
  std::map<std::string, std::string> headers;  // lower-case keys
  std::string body;
};

bool ReadRequest(int fd, Request* req) {
  std::string data;
  char buf[4096];
  size_t header_end = std::string::npos;
  while (header_end == std::string::npos) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) return false;
    data.append(buf, static_cast<size_t>(n));
    header_end = data.find("\r\n\r\n");
    if (data.size() > 65536) return false;
  }
  std::istringstream head(data.substr(0, header_end));
  std::string line;
  if (!std::getline(head, line)) return false;
  if (!line.empty() && line.back() == '\r') line.pop_back();
  std::istringstream request_line(line);
  std::string target, version;
  request_line >> req->method >> target >> version;
  const auto q = target.find('?');
  req->path = q == std::string::npos ? target : target.substr(0, q);
  req->query = q == std::string::npos ? "" : target.substr(q + 1);
  while (std::getline(head, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = line.substr(0, colon);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    req->headers[key] = Trim(line.substr(colon + 1));
  }
  size_t content_length = 0;
  if (auto it = req->headers.find("content-length"); it != req->headers.end()) content_length = std::stoul(it->second);
  req->body = data.substr(header_end + 4);
  while (req->body.size() < content_length) {
    const ssize_t n = ::recv(fd, buf, std::min(sizeof(buf), content_length - req->body.size()), 0);
    if (n <= 0) return false;
    req->body.append(buf, static_cast<size_t>(n));
  }
  return true;
}

void Respond(int fd, int status, const std::string& content_type, const std::string& body,
             const std::string& extra_headers = "") {
  const char* reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 404 ? "Not Found"
                     : status == 500 ? "Internal Server Error" : status == 504 ? "Gateway Timeout" : "Error";
  std::string head = fmt::format("HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nCache-Control: no-cache\r\n"
                                 "Access-Control-Allow-Origin: *\r\nConnection: close\r\n{}\r\n",
                                 status, reason, content_type, body.size(), extra_headers);
  if (SendAll(fd, head)) SendAll(fd, body);
}

void RespondJson(int fd, int status, const std::string& json) { Respond(fd, status, "application/json", json); }

}  // namespace

struct PreviewServer::Impl {
  struct Worker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  int listen_fd = -1;
  std::atomic<bool> running{false};
  std::thread accept_thread;
  std::mutex workers_mutex;
  std::vector<Worker> workers;

  // Joins the connection threads that have finished (a finished std::thread is still joinable, so
  // completion is tracked with a flag); with `all`, joins every thread.
  void Reap(bool all) {
    std::vector<Worker> to_join;
    {
      std::lock_guard<std::mutex> lock(workers_mutex);
      for (auto it = workers.begin(); it != workers.end();) {
        if (all || it->done->load()) {
          to_join.push_back(std::move(*it));
          it = workers.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto& w : to_join) {
      if (w.thread.joinable()) w.thread.join();
    }
  }
};

PreviewServer::PreviewServer(PreviewServerOptions options, std::shared_ptr<PreviewSink> sink, std::shared_ptr<Controls> controls)
    : options_(std::move(options)), sink_(std::move(sink)), controls_(std::move(controls)), impl_(std::make_unique<Impl>()) {
  if (!sink_) throw std::invalid_argument("PreviewServer: null sink");
  if (!controls_) throw std::invalid_argument("PreviewServer: null controls");
}

PreviewServer::~PreviewServer() { Stop(); }

void PreviewServer::Start() {
  if (impl_->running) return;
  std::error_code ec;
  std::filesystem::create_directories(options_.still_dir, ec);
  impl_->listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (impl_->listen_fd < 0) throw std::runtime_error(fmt::format("socket: {}", std::strerror(errno)));
  int one = 1;
  ::setsockopt(impl_->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(options_.port);
  if (::inet_pton(AF_INET, options_.bind_address.c_str(), &addr.sin_addr) != 1) {
    ::close(impl_->listen_fd);
    impl_->listen_fd = -1;
    throw std::runtime_error(fmt::format("bad bind address '{}'", options_.bind_address));
  }
  if (::bind(impl_->listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    const std::string err = std::strerror(errno);
    ::close(impl_->listen_fd);
    impl_->listen_fd = -1;
    throw std::runtime_error(fmt::format("bind {}:{} failed: {}", options_.bind_address, options_.port, err));
  }
  if (::listen(impl_->listen_fd, 16) != 0) {
    const std::string err = std::strerror(errno);
    ::close(impl_->listen_fd);
    impl_->listen_fd = -1;
    throw std::runtime_error(fmt::format("listen failed: {}", err));
  }
  sockaddr_in bound{};
  socklen_t len = sizeof(bound);
  if (::getsockname(impl_->listen_fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0) options_.port = ntohs(bound.sin_port);
  impl_->running = true;
  impl_->accept_thread = std::thread([this] {
    while (impl_->running) {
      sockaddr_in peer{};
      socklen_t peer_len = sizeof(peer);
      const int fd = ::accept(impl_->listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
      if (fd < 0) {
        if (!impl_->running) break;
        continue;
      }
      timeval tv{5, 0};
      ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      int nodelay = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
      impl_->Reap(false);
      auto done = std::make_shared<std::atomic<bool>>(false);
      std::thread thread([this, fd, done] {
        try {
          HandleConnection(fd);
        } catch (const std::exception& e) {
          HOLOSCAN_LOG_WARN("PreviewServer: connection error: {}", e.what());
        }
        ::close(fd);
        done->store(true);
      });
      std::lock_guard<std::mutex> lock(impl_->workers_mutex);
      impl_->workers.push_back({std::move(thread), std::move(done)});
    }
  });
  HOLOSCAN_LOG_INFO("PreviewServer: listening on http://{}:{}/ (stills in {})", options_.bind_address, options_.port, options_.still_dir);
}

void PreviewServer::Stop() {
  if (!impl_->running) return;
  impl_->running = false;
  if (impl_->listen_fd >= 0) {
    ::shutdown(impl_->listen_fd, SHUT_RDWR);
    ::close(impl_->listen_fd);
    impl_->listen_fd = -1;
  }
  if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
  impl_->Reap(true);
}

void PreviewServer::HandleConnection(int fd) {
  Request req;
  if (!ReadRequest(fd, &req)) return;

  if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
    std::string page(PreviewPageHtml());
    const std::string marker = "__TITLE__";
    for (size_t pos = page.find(marker); pos != std::string::npos; pos = page.find(marker, pos)) page.replace(pos, marker.size(), options_.title);
    Respond(fd, 200, "text/html; charset=utf-8", page);
    return;
  }
  if (req.method == "GET" && req.path == "/stream.mjpg") {
    if (!SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                     "Cache-Control: no-cache\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n")) {
      return;
    }
    uint64_t last = 0;
    const auto min_interval = options_.stream_fps > 0 ? std::chrono::duration<double>(1.0 / options_.stream_fps)
                                                      : std::chrono::duration<double>(0);
    auto last_sent = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    while (impl_->running) {
      auto frame = sink_->WaitForNewer(last, std::chrono::milliseconds(500));
      if (!frame) continue;
      last = frame->sequence;
      const auto now = std::chrono::steady_clock::now();
      if (now - last_sent < min_interval) std::this_thread::sleep_for(min_interval - (now - last_sent));
      last_sent = std::chrono::steady_clock::now();
      const std::string part = fmt::format("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: {}\r\n\r\n", frame->jpeg.size());
      if (!SendAll(fd, part) || !SendAll(fd, frame->jpeg.data(), frame->jpeg.size()) || !SendAll(fd, "\r\n")) break;
    }
    return;
  }
  if (req.method == "GET" && req.path == "/snapshot.jpg") {
    auto frame = sink_->Latest();
    if (!frame) frame = sink_->WaitForNewer(0, std::chrono::milliseconds(2000));
    if (!frame) {
      RespondJson(fd, 504, "{\"error\":\"no frame yet\"}");
      return;
    }
    Respond(fd, 200, "image/jpeg", std::string(frame->jpeg.begin(), frame->jpeg.end()));
    return;
  }
  if (req.method == "GET" && req.path == "/still.jpg") {
    const uint64_t before = sink_->LatestStill() ? sink_->LatestStill()->sequence : 0;
    sink_->RequestStill();
    auto still = sink_->WaitForStill(before, std::chrono::milliseconds(5000));
    if (!still) {
      RespondJson(fd, 504, "{\"error\":\"no still within 5 s (is the pipeline running?)\"}");
      return;
    }
    std::string camera = "camera";
    try {
      camera = controls_->Get().camera;
    } catch (const std::exception&) {
    }
    if (camera.empty()) camera = "camera";
    const std::string name = fmt::format("{}_{}.jpg", camera, Timestamp());
    {
      std::ofstream out(options_.still_dir + "/" + name, std::ios::binary);
      out.write(reinterpret_cast<const char*>(still->jpeg.data()), static_cast<std::streamsize>(still->jpeg.size()));
    }
    Respond(fd, 200, "image/jpeg", std::string(still->jpeg.begin(), still->jpeg.end()),
            fmt::format("X-File: {}\r\nContent-Disposition: inline; filename=\"{}\"\r\n", name, name));
    return;
  }
  if (req.method == "GET" && req.path == "/status.json") {
    std::string status;
    ControlState state;
    try {
      state = controls_->Get();
      status = Trim(controls_->StatusJson());
    } catch (const std::exception& e) {
      RespondJson(fd, 500, fmt::format("{{\"error\":\"{}\"}}", JsonEscape(e.what())));
      return;
    }
    if (status.size() >= 2 && status.front() == '{' && status.back() == '}') status = Trim(status.substr(1, status.size() - 2));
    std::string json = fmt::format(
        "{{\"camera\":\"{}\",\"mode\":\"{}\",\"width\":{},\"height\":{},\"fps\":{:.4f},\"exposure_ms\":{:.4f},\"gain_db\":{:.2f},"
        "\"black_level\":{},\"test_pattern\":{},\"test_pattern_select\":{},\"exposure_max_ms\":{:.4f},\"gain_max_db\":{:.2f},"
        "\"stream_frames\":{}",
        JsonEscape(state.camera), JsonEscape(state.mode), state.width, state.height, state.fps, state.exposure_ms, state.gain_db,
        state.black_level, state.test_pattern ? "true" : "false", state.test_pattern_select, state.exposure_max_ms,
        state.gain_max_db, sink_->frames_published());
    if (!status.empty()) json += "," + status;
    json += "}";
    RespondJson(fd, 200, json);
    return;
  }
  if (req.method == "POST" && req.path == "/control") {
    const std::string body = Trim(req.body);
    const auto kv = (!body.empty() && body.front() == '{') ? ParseFlatJson(body) : ParseForm(body);
    if (kv.empty()) {
      RespondJson(fd, 400, "{\"ok\":false,\"error\":\"no key/value pairs\"}");
      return;
    }
    std::string error;
    try {
      error = controls_->Apply(kv);
    } catch (const std::exception& e) {
      error = e.what();
    }
    if (error.empty()) RespondJson(fd, 200, "{\"ok\":true}");
    else RespondJson(fd, 400, fmt::format("{{\"ok\":false,\"error\":\"{}\"}}", JsonEscape(error)));
    return;
  }
  if (req.method == "POST" && req.path == "/capture") {
    std::string path;
    try {
      path = controls_->CaptureRaw();
    } catch (const std::exception& e) {
      path = std::string("error: ") + e.what();
    }
    if (path.rfind("error:", 0) == 0) RespondJson(fd, 500, fmt::format("{{\"ok\":false,\"error\":\"{}\"}}", JsonEscape(path)));
    else RespondJson(fd, 200, fmt::format("{{\"ok\":true,\"path\":\"{}\"}}", JsonEscape(path)));
    return;
  }
  if (req.method == "GET" && req.path == "/files.json") {
    struct Entry { std::string name; uintmax_t size; int64_t mtime; };
    std::vector<Entry> entries;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(options_.still_dir, ec)) {
      if (!e.is_regular_file(ec)) continue;
      struct stat st{};
      if (::stat(e.path().c_str(), &st) != 0) continue;
      entries.push_back({e.path().filename().string(), static_cast<uintmax_t>(st.st_size), static_cast<int64_t>(st.st_mtime)});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.mtime > b.mtime; });
    std::string json = "[";
    for (size_t i = 0; i < entries.size(); ++i) {
      json += fmt::format("{}{{\"name\":\"{}\",\"size\":{},\"mtime\":{}}}", i ? "," : "", JsonEscape(entries[i].name), entries[i].size,
                          entries[i].mtime);
    }
    json += "]";
    RespondJson(fd, 200, json);
    return;
  }
  if (req.method == "GET" && req.path.rfind("/files/", 0) == 0) {
    const std::string name = UrlDecode(req.path.substr(7));
    if (name.empty() || name.find('/') != std::string::npos || name.find("..") != std::string::npos) {
      RespondJson(fd, 400, "{\"error\":\"bad file name\"}");
      return;
    }
    std::ifstream in(options_.still_dir + "/" + name, std::ios::binary);
    if (!in) {
      RespondJson(fd, 404, "{\"error\":\"not found\"}");
      return;
    }
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string type = ContentTypeFor(name);
    const std::string disposition = type == "application/octet-stream" ? fmt::format("Content-Disposition: attachment; filename=\"{}\"\r\n", name) : "";
    Respond(fd, 200, type, content, disposition);
    return;
  }
  RespondJson(fd, 404, fmt::format("{{\"error\":\"no route for {} {}\"}}", JsonEscape(req.method), JsonEscape(req.path)));
}

}  // namespace hsb::preview
