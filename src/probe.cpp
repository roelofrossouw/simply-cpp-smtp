#include <sc/smtp/probe.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <resolv.h>
#include <sys/socket.h>
#include <unistd.h>

namespace sc::smtp {
    namespace {
        using clock = std::chrono::steady_clock;

        std::string without_trailing_dot(std::string name) {
            while (!name.empty() && name.back() == '.') name.pop_back();
            return name;
        }

        std::string lower(std::string text) {
            std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        // A resolver state of its own per lookup: res_query's shared one isn't for threads everywhere.
        class resolver {
        public:
            resolver() { ready_ = res_ninit(&state_) == 0; }
            ~resolver() {
                if (ready_) res_nclose(&state_);
            }
            resolver(const resolver &) = delete;
            resolver &operator=(const resolver &) = delete;

            // The MX records' (preference, exchange); nullopt when the lookup failed (NXDOMAIN, no answer, no server)
            std::optional<std::vector<std::pair<int, std::string>>> mx(const std::string &domain) {
                if (!ready_) return std::nullopt;
                unsigned char answer[NS_MAXMSG];
                const int length = res_nquery(&state_, domain.c_str(), ns_c_in, ns_t_mx, answer, sizeof answer);
                if (length < 0) return std::nullopt;
                ns_msg message{};
                if (ns_initparse(answer, length, &message) < 0) return std::nullopt;
                std::vector<std::pair<int, std::string>> found;
                const int count = ns_msg_count(message, ns_s_an);
                for (int i = 0; i < count; ++i) {
                    ns_rr record{};
                    if (ns_parserr(&message, ns_s_an, i, &record) < 0) continue;
                    if (ns_rr_type(record) != ns_t_mx || ns_rr_rdlen(record) < 3) continue;
                    const unsigned char *data = ns_rr_rdata(record);
                    const int preference = data[0] << 8 | data[1];
                    char exchange[NS_MAXDNAME] = {};
                    if (dn_expand(ns_msg_base(message), ns_msg_end(message), data + 2, exchange, sizeof exchange) < 0) continue;
                    found.emplace_back(preference, exchange);
                }
                return found;
            }

        private:
            struct __res_state state_{};
            bool ready_ = false;
        };

        class connection {
        public:
            connection() = default;
            ~connection() { close(); }
            connection(const connection &) = delete;
            connection &operator=(const connection &) = delete;

            void close() {
                if (socket_ >= 0) ::close(socket_);
                socket_ = -1;
                buffer_.clear();
            }

            // Connects to one of host's addresses, IPv4 first; "" when connected, else why not
            std::string open(const std::string &host, const int port, const std::chrono::milliseconds timeout, const clock::time_point deadline) {
                addrinfo hints{};
                hints.ai_family = AF_UNSPEC;
                hints.ai_socktype = SOCK_STREAM;
                addrinfo *found = nullptr;
                const auto port_text = std::to_string(port);
                if (const int failed = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &found); failed != 0)
                    return gai_strerror(failed);
                std::vector<const addrinfo *> addresses;
                for (const addrinfo *one = found; one; one = one->ai_next) addresses.push_back(one);
                std::ranges::stable_partition(addresses, [](const addrinfo *one) { return one->ai_family == AF_INET; });
                std::string why = "no address";
                for (const addrinfo *one: addresses) {
                    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now());
                    if (left <= std::chrono::milliseconds::zero()) {
                        why = "timed out";
                        break;
                    }
                    why = attempt(one, std::min(timeout, left));
                    if (why.empty()) break;
                }
                freeaddrinfo(found);
                return why;
            }

            // One reply, all its lines ("250-a", "250 b"): its code (0 when it couldn't be read: closed, timed out,
            // not SMTP) and its lines joined by "\n"; why says what went wrong
            std::pair<int, std::string> reply(const clock::time_point until, std::string &why) {
                std::string text;
                while (true) {
                    const auto end = buffer_.find('\n');
                    if (end == std::string::npos) {
                        if (!fill(until, why)) return {0, text};
                        continue;
                    }
                    std::string line = buffer_.substr(0, end);
                    buffer_.erase(0, end + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!text.empty()) text += '\n';
                    text += line;
                    if (line.size() < 3 || !std::isdigit(static_cast<unsigned char>(line[0])) ||
                        !std::isdigit(static_cast<unsigned char>(line[1])) || !std::isdigit(static_cast<unsigned char>(line[2]))) {
                        why = "not an SMTP reply";
                        return {0, text};
                    }
                    if (line.size() == 3 || line[3] != '-') return {std::stoi(line.substr(0, 3)), text};
                }
            }

            bool say(const std::string &command, const clock::time_point until, std::string &why) {
                const std::string line = command + "\r\n";
                std::size_t sent = 0;
                while (sent < line.size()) {
                    if (!wait(POLLOUT, until, why)) return false;
                    const auto n = ::send(socket_, line.data() + sent, line.size() - sent, send_flags);
                    if (n < 0) {
                        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                        why = std::strerror(errno);
                        return false;
                    }
                    sent += static_cast<std::size_t>(n);
                }
                return true;
            }

        private:
#ifdef MSG_NOSIGNAL
            static constexpr int send_flags = MSG_NOSIGNAL;
#else
            static constexpr int send_flags = 0;
#endif

            std::string attempt(const addrinfo *address, const std::chrono::milliseconds timeout) {
                close();
                socket_ = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
                if (socket_ < 0) return std::strerror(errno);
#ifdef SO_NOSIGPIPE
                const int yes = 1;
                setsockopt(socket_, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif
                fcntl(socket_, F_SETFL, fcntl(socket_, F_GETFL, 0) | O_NONBLOCK);
                if (::connect(socket_, address->ai_addr, address->ai_addrlen) == 0) return {};
                if (errno != EINPROGRESS) {
                    std::string why = std::strerror(errno);
                    close();
                    return why;
                }
                std::string why;
                if (!wait(POLLOUT, clock::now() + timeout, why)) {
                    close();
                    return why;
                }
                int error = 0;
                socklen_t size = sizeof error;
                getsockopt(socket_, SOL_SOCKET, SO_ERROR, &error, &size);
                if (error != 0) {
                    close();
                    return std::strerror(error);
                }
                return {};
            }

            bool wait(const short events, const clock::time_point until, std::string &why) {
                while (true) {
                    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - clock::now()).count();
                    if (left <= 0) {
                        why = "timed out";
                        return false;
                    }
                    pollfd one{socket_, events, 0};
                    const int ready = ::poll(&one, 1, static_cast<int>(left));
                    if (ready > 0) return true;
                    if (ready < 0 && errno != EINTR) {
                        why = std::strerror(errno);
                        return false;
                    }
                }
            }

            bool fill(const clock::time_point until, std::string &why) {
                if (buffer_.size() > 64 * 1024) {
                    why = "reply too long";
                    return false;
                }
                if (!wait(POLLIN, until, why)) return false;
                char chunk[4096];
                const auto n = ::recv(socket_, chunk, sizeof chunk, 0);
                if (n == 0) {
                    why = "connection closed";
                    return false;
                }
                if (n < 0) {
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return true;
                    why = std::strerror(errno);
                    return false;
                }
                buffer_.append(chunk, static_cast<std::size_t>(n));
                return true;
            }

            int socket_ = -1;
            std::string buffer_;
        };

        void add_problem(std::string &problem, const std::string &host, const std::string &why) {
            if (!problem.empty()) problem += "; ";
            problem += host + ": " + why;
        }
    }

    std::vector<std::string> mail_exchangers(const std::string_view domain) {
        const auto name = without_trailing_dot(std::string{domain});
        resolver dns;
        auto records = dns.mx(name);
        if (!records || records->empty()) return {name};
        std::ranges::stable_sort(*records, {}, &std::pair<int, std::string>::first);
        std::vector<std::string> hosts;
        for (auto &[preference, exchange]: *records) {
            auto host = without_trailing_dot(exchange);
            if (host.empty()) {
                // A null MX (RFC 7505): "." with preference 0, alone; the domain takes no mail
                if (records->size() == 1) return {};
                continue;
            }
            if (std::ranges::find(hosts, host) == hosts.end()) hosts.push_back(std::move(host));
        }
        return hosts;
    }

    probe_result probe(const std::string_view address, const probe_settings &how, std::vector<std::string> hosts) {
        probe_result result;
        const auto started = clock::now();
        const auto deadline = started + how.total_timeout;
        if (hosts.empty()) {
            const auto at = address.rfind('@');
            const auto domain = at == std::string_view::npos ? std::string{} : std::string{address.substr(at + 1)};
            if (domain.empty()) {
                result.problem = "no domain";
                return result;
            }
            hosts = mail_exchangers(domain);
            if (hosts.empty()) {
                result.stage = probe_stage::connect;
                result.problem = domain + " takes no mail (null MX)";
                return result;
            }
        }
        const auto helo = how.helo.empty() ? host_name() : how.helo;
        const auto command_until = [&] { return std::min(clock::now() + how.command_timeout, deadline); };

        for (const auto &host: hosts) {
            if (clock::now() >= deadline) {
                add_problem(result.problem, host, "not tried: out of time");
                break;
            }
            result.stage = probe_stage::connect;
            connection server;
            if (auto why = server.open(host, how.port, how.connect_timeout, deadline); !why.empty()) {
                add_problem(result.problem, host, why);
                continue;
            }
            std::string why;
            auto [code, text] = server.reply(command_until(), why);
            if (code != 220) {
                add_problem(result.problem, host, code == 0 ? why : "greeted with " + text);
                continue;
            }
            result.host = host;

            // EHLO, or HELO for a server that doesn't know it; a refusal of both ends it (a 5xx is an answer)
            std::tie(code, text) = server.say("EHLO " + helo, command_until(), why) ? server.reply(command_until(), why) : std::pair{0, std::string{}};
            if (code >= 500 && code < 600) {
                std::tie(code, text) = server.say("HELO " + helo, command_until(), why) ? server.reply(command_until(), why) : std::pair{0, std::string{}};
            }
            if (code / 100 != 2) {
                if (code == 0) {
                    add_problem(result.problem, host, why);
                    continue;
                }
                result = {code, text, host, probe_stage::ehlo, result.problem};
                (void) server.say("QUIT", command_until(), why);
                return result;
            }

            std::tie(code, text) = server.say("MAIL FROM:<" + how.from + ">", command_until(), why) ? server.reply(command_until(), why) : std::pair{0, std::string{}};
            if (code / 100 != 2) {
                if (code == 0) {
                    add_problem(result.problem, host, why);
                    continue;
                }
                result = {code, text, host, probe_stage::mail, result.problem};
                (void) server.say("QUIT", command_until(), why);
                return result;
            }

            std::tie(code, text) = server.say("RCPT TO:<" + std::string{address} + ">", command_until(), why) ? server.reply(command_until(), why) : std::pair{0, std::string{}};
            if (code == 0) {
                add_problem(result.problem, host, why);
                continue;
            }
            result = {code, text, host, probe_stage::rcpt, result.problem};
            // Polite ending; what the server says to it doesn't matter, and the deadline isn't waited for
            const auto ending = std::min(clock::now() + std::chrono::seconds{5}, deadline);
            if (server.say("RSET", ending, why)) (void) server.reply(ending, why);
            if (server.say("QUIT", ending, why)) (void) server.reply(ending, why);
            return result;
        }
        result.code = 0;
        result.reply.clear();
        if (result.stage == probe_stage::none) result.stage = probe_stage::connect;
        return result;
    }

    std::string host_name() {
        char name[256] = {};
        if (gethostname(name, sizeof name - 1) != 0) return "localhost";
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_flags = AI_CANONNAME;
        addrinfo *found = nullptr;
        std::string result = name;
        if (getaddrinfo(name, nullptr, &hints, &found) == 0) {
            if (found && found->ai_canonname && std::strchr(found->ai_canonname, '.')) result = found->ai_canonname;
            freeaddrinfo(found);
        }
        return lower(result);
    }

    std::string_view stage_name(const probe_stage stage) {
        switch (stage) {
            case probe_stage::connect: return "connect";
            case probe_stage::ehlo: return "ehlo";
            case probe_stage::mail: return "mail";
            case probe_stage::rcpt: return "rcpt";
            case probe_stage::none: break;
        }
        return "none";
    }
}
