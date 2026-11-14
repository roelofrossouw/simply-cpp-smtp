// sc::smtp::client against a small SMTP server in the test (127.0.0.1, a free port, a thread): the envelope and the
// text as the server gets them ("\r\n" line ends, dot-stuffing), EHLO's name, the null sender, refused recipients and
// senders (with their reply codes, and whether they're temporary), no server there, STARTTLS required by a server
// that doesn't offer it; address_of(), security_from(), default_sender().
#include <sc_smtp.h>
#include <sc_test.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
    // One connection at a time: what it was told, in order; MAIL FROM/RCPT TO starting with "bad" get 550, "busy" 451
    class fake_server {
    public:
        fake_server() {
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
        std::vector<std::string> commands;
        std::string data;

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
            say(c, "220 test ESMTP\r\n");
            std::string buffer;
            bool in_data = false;
            char chunk[4096];
            while (true) {
                const auto n = recv(c, chunk, sizeof chunk, 0);
                if (n <= 0) return;
                buffer.append(chunk, static_cast<std::size_t>(n));
                while (true) {
                    if (in_data) {
                        const auto end = buffer.find("\r\n.\r\n");
                        if (end == std::string::npos) break;
                        data = buffer.substr(0, end + 2);
                        buffer.erase(0, end + 5);
                        in_data = false;
                        say(c, "250 2.0.0 queued\r\n");
                        continue;
                    }
                    const auto end = buffer.find("\r\n");
                    if (end == std::string::npos) break;
                    const auto line = buffer.substr(0, end);
                    buffer.erase(0, end + 2);
                    commands.push_back(line);
                    const auto has = [&](const char *start) { return line.rfind(start, 0) == 0; };
                    if (has("EHLO")) say(c, "250-test\r\n250 8BITMIME\r\n");
                    else if (has("MAIL FROM:<bad") || has("RCPT TO:<bad")) say(c, "550 5.1.1 no such user\r\n");
                    else if (has("MAIL FROM:<busy") || has("RCPT TO:<busy")) say(c, "451 4.3.0 try later\r\n");
                    else if (has("MAIL") || has("RCPT") || has("RSET") || has("NOOP")) say(c, "250 OK\r\n");
                    else if (has("DATA")) {
                        say(c, "354 go on\r\n");
                        in_data = true;
                    } else if (has("QUIT")) {
                        say(c, "221 bye\r\n");
                        return;
                    } else say(c, "502 not here\r\n");
                }
            }
        }

        int listener_ = -1, port_ = 0;
        std::atomic<bool> stop_{false};
        std::thread thread_;
    };

    bool has_command(const std::vector<std::string> &commands, const std::string &command) {
        for (const auto &one: commands)
            if (one == command) return true;
        return false;
    }
}

int main() {
    SECTION("address_of, security_from");
    CHECK(sc::smtp::address_of("Ann Smith <ann@example.com>") == "ann@example.com");
    CHECK(sc::smtp::address_of("  ann@example.com ") == "ann@example.com");
    CHECK(sc::smtp::address_of("\"Smith, Ann\" < ann@example.com >") == "ann@example.com");
    CHECK(sc::smtp::security_from("starttls_required") == sc::smtp::security::starttls_required);
    bool refused = false;
    try {
        (void) sc::smtp::security_from("ssl");
    } catch (const std::invalid_argument &) { refused = true; }
    CHECK(refused);
    CHECK(sc::smtp::default_sender().find('@') != std::string::npos || !sc::smtp::default_sender().empty());

    SECTION("a message as the server gets it");
    {
        fake_server server;
        const sc::smtp::client smtp{{.host = "127.0.0.1", .port = server.port(), .tls = sc::smtp::security::none, .helo = "mirror.example.com",
                                     .connect_timeout = std::chrono::seconds{5}, .timeout = std::chrono::seconds{10}}};
        smtp.send({.from = "reports@example.com", .to = {"ann@example.com", "bob@example.com"}},
                  "From: reports@example.com\nTo: ann@example.com\nSubject: Test\n\nLine one\n.hidden line\r\nlast");
        CHECK(has_command(server.commands, "EHLO mirror.example.com"));
        CHECK(has_command(server.commands, "MAIL FROM:<reports@example.com>"));
        CHECK(has_command(server.commands, "RCPT TO:<ann@example.com>"));
        CHECK(has_command(server.commands, "RCPT TO:<bob@example.com>"));
        CHECK(server.data == "From: reports@example.com\r\nTo: ann@example.com\r\nSubject: Test\r\n\r\nLine one\r\n..hidden line\r\nlast\r\n");
    }

    SECTION("the null sender");
    {
        fake_server server;
        const sc::smtp::client smtp{{.host = "127.0.0.1", .port = server.port(), .tls = sc::smtp::security::none}};
        smtp.send({.from = "", .to = {"ann@example.com"}}, "Subject: bounce\n\nx\n");
        bool found = false;
        for (const auto &command: server.commands)
            if (command.rfind("MAIL FROM:<>", 0) == 0) found = true;
        CHECK(found);
    }

    SECTION("refused: a recipient (550, permanent), the sender for now (451, temporary)");
    {
        fake_server server;
        const sc::smtp::client smtp{{.host = "127.0.0.1", .port = server.port(), .tls = sc::smtp::security::none}};
        int code = -1;
        bool temporary = true;
        try {
            smtp.send({.from = "reports@example.com", .to = {"ann@example.com", "bad@example.com"}}, "Subject: x\n\nx\n");
        } catch (const sc::smtp::error &error) {
            code = error.code();
            temporary = error.temporary();
        }
        CHECK(code == 550);
        CHECK(!temporary);
        CHECK(server.data.empty());

        code = -1;
        try {
            smtp.send({.from = "busy@example.com", .to = {"ann@example.com"}}, "Subject: x\n\nx\n");
        } catch (const sc::smtp::error &error) {
            code = error.code();
            temporary = error.temporary();
        }
        CHECK(code == 451);
        CHECK(temporary);
    }

    SECTION("STARTTLS required, not offered");
    {
        fake_server server;
        const sc::smtp::client smtp{{.host = "127.0.0.1", .port = server.port(), .tls = sc::smtp::security::starttls_required}};
        bool failed = false;
        try {
            smtp.send({.from = "reports@example.com", .to = {"ann@example.com"}}, "Subject: x\n\nx\n");
        } catch (const sc::smtp::error &) { failed = true; }
        CHECK(failed);
        CHECK(server.data.empty());
    }

    SECTION("no server there; bad addresses");
    {
        int port = 0;
        {
            fake_server gone;
            port = gone.port();
        }
        const sc::smtp::client smtp{{.host = "127.0.0.1", .port = port, .tls = sc::smtp::security::none, .connect_timeout = std::chrono::seconds{5}}};
        bool temporary = false;
        try {
            smtp.send({.from = "reports@example.com", .to = {"ann@example.com"}}, "x");
        } catch (const sc::smtp::error &error) { temporary = error.temporary() && error.code() == 0; }
        CHECK(temporary);

        bool refused = false;
        try {
            smtp.send({.from = "reports@example.com", .to = {"ann@example.com>\r\nRCPT TO:<x@example.com"}}, "x");
        } catch (const sc::smtp::error &) { refused = true; }
        CHECK(refused);
        refused = false;
        try {
            smtp.send({.from = "reports@example.com", .to = {}}, "x");
        } catch (const sc::smtp::error &) { refused = true; }
        CHECK(refused);
    }

    TEST_SUMMARY();
}
