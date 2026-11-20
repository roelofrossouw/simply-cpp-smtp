// sc::smtp::probe() against a small SMTP server in the test (127.0.0.1, a free port, a thread): the conversation it
// has (EHLO with the given name, MAIL FROM, RCPT TO, RSET, QUIT, never DATA), each kind of answer (taken, refused,
// greylisted), a server that refuses EHLO (HELO instead) or the sender, one that greets with 554 or not at all (the
// next host is asked), a host that doesn't resolve, the total timeout; mail_exchangers() for a domain without MX.
#include <sc/smtp/probe.h>
#include <sc_test.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
    enum class mood { normal, no_ehlo, refuse_sender, greet_554, silent };

    // RCPT TO:<ann...> 250, <bad...> 550 5.1.1, <grey...> 451, <proto...> 550 5.5.1 Protocol error, <multi...> a
    // two-line 550
    class fake_server {
    public:
        explicit fake_server(const mood how = mood::normal) : mood_{how} {
            listener_ = socket(AF_INET, SOCK_STREAM, 0);
            const int yes = 1;
            setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
            sockaddr_in where{};
            where.sin_family = AF_INET;
            where.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            bind(listener_, reinterpret_cast<sockaddr *>(&where), sizeof where);
            listen(listener_, 4);
            socklen_t size = sizeof where;
            getsockname(listener_, reinterpret_cast<sockaddr *>(&where), &size);
            port_ = ntohs(where.sin_port);
            thread_ = std::thread{[this] { serve(); }};
        }

        ~fake_server() {
            stop_ = true;
            shutdown(listener_, SHUT_RDWR);
            close(listener_);
            thread_.join();
        }

        [[nodiscard]] int port() const { return port_; }

        [[nodiscard]] std::vector<std::string> commands() {
            const std::lock_guard lock{mutex_};
            return commands_;
        }

    private:
        void serve() {
            while (!stop_) {
                const int c = accept(listener_, nullptr, nullptr);
                if (c < 0) return;
                talk(c);
                close(c);
            }
        }

        static void say(const int c, const std::string &line) { (void) ::send(c, line.data(), line.size(), 0); }

        void talk(const int c) {
            if (mood_ == mood::silent) {
                char chunk[256];
                while (!stop_ && recv(c, chunk, sizeof chunk, 0) > 0) {}
                return;
            }
            if (mood_ == mood::greet_554) {
                say(c, "554 5.7.1 go away\r\n");
                return;
            }
            say(c, "220-test ESMTP\r\n220 welcome\r\n");
            std::string buffer;
            char chunk[4096];
            while (true) {
                const auto n = recv(c, chunk, sizeof chunk, 0);
                if (n <= 0) return;
                buffer.append(chunk, static_cast<std::size_t>(n));
                while (true) {
                    const auto end = buffer.find("\r\n");
                    if (end == std::string::npos) break;
                    const auto line = buffer.substr(0, end);
                    buffer.erase(0, end + 2);
                    {
                        const std::lock_guard lock{mutex_};
                        commands_.push_back(line);
                    }
                    const auto has = [&](const char *start) { return line.rfind(start, 0) == 0; };
                    if (has("EHLO")) say(c, mood_ == mood::no_ehlo ? "502 5.5.2 what?\r\n" : "250-test\r\n250 8BITMIME\r\n");
                    else if (has("HELO")) say(c, "250 test\r\n");
                    else if (has("MAIL FROM:")) say(c, mood_ == mood::refuse_sender ? "553 5.7.1 sender rejected\r\n" : "250 2.1.0 OK\r\n");
                    else if (has("RCPT TO:<ann")) say(c, "250 2.1.5 OK\r\n");
                    else if (has("RCPT TO:<bad")) say(c, "550 5.1.1 No such user\r\n");
                    else if (has("RCPT TO:<grey")) say(c, "451 4.7.1 Greylisted, try later\r\n");
                    else if (has("RCPT TO:<proto")) say(c, "550 5.5.1 Protocol error\r\n");
                    else if (has("RCPT TO:<multi")) say(c, "550-5.1.1 The mailbox\r\n550 5.1.1 does not exist\r\n");
                    else if (has("RSET")) say(c, "250 2.0.0 OK\r\n");
                    else if (has("QUIT")) {
                        say(c, "221 bye\r\n");
                        return;
                    } else say(c, "502 5.5.2 not here\r\n");
                }
            }
        }

        mood mood_;
        int listener_ = -1, port_ = 0;
        std::atomic<bool> stop_{false};
        std::thread thread_;
        std::mutex mutex_;
        std::vector<std::string> commands_;
    };

    sc::smtp::probe_settings settings_for(const fake_server &server) {
        return {.from = "checks@example.net", .helo = "probe.example.net", .port = server.port(),
                .connect_timeout = std::chrono::milliseconds{2000}, .command_timeout = std::chrono::milliseconds{2000},
                .total_timeout = std::chrono::milliseconds{5000}};
    }

    bool has_command(const std::vector<std::string> &commands, const std::string &command) {
        for (const auto &one: commands)
            if (one == command) return true;
        return false;
    }

    bool starts_with_any(const std::vector<std::string> &commands, const std::string &start) {
        for (const auto &one: commands)
            if (one.rfind(start, 0) == 0) return true;
        return false;
    }
}

int main() {
    SECTION("an address taken: the whole conversation, never DATA");
    {
        fake_server server;
        const auto answer = sc::smtp::probe("ann@example.com", settings_for(server), {"127.0.0.1"});
        CHECK_EQ(answer.code, 250);
        CHECK(answer.reply == "250 2.1.5 OK");
        CHECK(answer.stage == sc::smtp::probe_stage::rcpt);
        CHECK(answer.host == "127.0.0.1");
        CHECK(answer.problem.empty());
        // The server has seen QUIT once probe() returns: it answers 221 before closing
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        const auto commands = server.commands();
        CHECK(has_command(commands, "EHLO probe.example.net"));
        CHECK(has_command(commands, "MAIL FROM:<checks@example.net>"));
        CHECK(has_command(commands, "RCPT TO:<ann@example.com>"));
        CHECK(has_command(commands, "RSET"));
        CHECK(has_command(commands, "QUIT"));
        CHECK(!starts_with_any(commands, "DATA"));
    }

    SECTION("refused, greylisted, the protocol error, a reply of two lines");
    {
        fake_server server;
        auto answer = sc::smtp::probe("bad@example.com", settings_for(server), {"127.0.0.1"});
        CHECK_EQ(answer.code, 550);
        CHECK(answer.reply == "550 5.1.1 No such user");
        answer = sc::smtp::probe("grey@example.com", settings_for(server), {"127.0.0.1"});
        CHECK_EQ(answer.code, 451);
        answer = sc::smtp::probe("proto@example.com", settings_for(server), {"127.0.0.1"});
        CHECK(answer.reply == "550 5.5.1 Protocol error");
        answer = sc::smtp::probe("multi@example.com", settings_for(server), {"127.0.0.1"});
        CHECK_EQ(answer.code, 550);
        CHECK(answer.reply == "550-5.1.1 The mailbox\n550 5.1.1 does not exist");
    }

    SECTION("the null sender");
    {
        fake_server server;
        auto how = settings_for(server);
        how.from.clear();
        (void) sc::smtp::probe("ann@example.com", how, {"127.0.0.1"});
        CHECK(has_command(server.commands(), "MAIL FROM:<>"));
    }

    SECTION("EHLO refused: HELO instead");
    {
        fake_server server{mood::no_ehlo};
        const auto answer = sc::smtp::probe("ann@example.com", settings_for(server), {"127.0.0.1"});
        CHECK_EQ(answer.code, 250);
        CHECK(has_command(server.commands(), "HELO probe.example.net"));
    }

    SECTION("the sender refused: that's the answer, no RCPT TO");
    {
        fake_server server{mood::refuse_sender};
        const auto answer = sc::smtp::probe("ann@example.com", settings_for(server), {"127.0.0.1"});
        CHECK_EQ(answer.code, 553);
        CHECK(answer.stage == sc::smtp::probe_stage::mail);
        CHECK(!starts_with_any(server.commands(), "RCPT"));
    }

    SECTION("a host that doesn't resolve, then one that answers");
    {
        fake_server server;
        const auto answer = sc::smtp::probe("ann@example.com", settings_for(server), {"nowhere.invalid", "127.0.0.1"});
        CHECK_EQ(answer.code, 250);
        CHECK(answer.host == "127.0.0.1");
        CHECK(answer.problem.find("nowhere.invalid: ") == 0);
    }

    SECTION("greeted with 554: the next host; none left: code 0 and why");
    {
        fake_server server{mood::greet_554};
        const auto answer = sc::smtp::probe("ann@example.com", settings_for(server), {"127.0.0.1"});
        CHECK_EQ(answer.code, 0);
        CHECK(answer.stage == sc::smtp::probe_stage::connect);
        CHECK(answer.problem.find("554 5.7.1 go away") != std::string::npos);
    }

    SECTION("nobody listening");
    {
        int port = 0;
        {
            fake_server gone;
            port = gone.port();
        }
        const auto answer = sc::smtp::probe("ann@example.com", {.port = port, .connect_timeout = std::chrono::milliseconds{1000}}, {"127.0.0.1"});
        CHECK_EQ(answer.code, 0);
        CHECK(answer.stage == sc::smtp::probe_stage::connect);
        CHECK(!answer.problem.empty());
    }

    SECTION("a server that never greets: the total timeout holds");
    {
        fake_server server{mood::silent};
        auto how = settings_for(server);
        how.command_timeout = std::chrono::milliseconds{5000};
        how.total_timeout = std::chrono::milliseconds{800};
        const auto start = std::chrono::steady_clock::now();
        const auto answer = sc::smtp::probe("ann@example.com", how, {"127.0.0.1"});
        const auto took = std::chrono::steady_clock::now() - start;
        CHECK_EQ(answer.code, 0);
        CHECK(answer.problem.find("timed out") != std::string::npos);
        CHECK(took < std::chrono::milliseconds{2000});
    }

    SECTION("mail_exchangers: a domain without MX is its own; host_name");
    {
        const auto hosts = sc::smtp::mail_exchangers("nowhere.invalid.");
        CHECK_EQ(hosts.size(), std::size_t{1});
        CHECK(!hosts.empty() && hosts[0] == "nowhere.invalid");
        CHECK(!sc::smtp::host_name().empty());
        CHECK(sc::smtp::stage_name(sc::smtp::probe_stage::rcpt) == "rcpt");
    }

    TEST_SUMMARY();
}
