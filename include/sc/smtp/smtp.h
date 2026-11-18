#ifndef SC_SMTP_H
#define SC_SMTP_H
// Sending mail over SMTP (RFC 5321), from the program itself: no sendmail, no other program run. A message (its
// text as sc::mime::draft writes it, or any RFC 5322 text) goes to a server (a relay or smarthost) with an envelope:
// who it's from and who gets it. STARTTLS when the server offers it, or always, or TLS from the start (port 465);
// signing in when given a user. libcurl does the protocol (and the dot-stuffing); nothing of it shows here.
//
//     const sc::smtp::client smtp{{.host = "smtp.example.com"}};
//     smtp.send({.from = "reports@example.com", .to = {"ann@example.com"}}, sc::mime::draft{}...str());

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sc::smtp {
    enum class security {
        none,         // plain text all the way
        starttls,     // STARTTLS when the server offers it, plain text when it doesn't (what MTAs do)
        starttls_required, // STARTTLS, or no mail
        tls           // TLS from the start (SMTPS, port 465)
    };

    struct settings {
        std::string host = "localhost";
        int port = 0;                               // 0: 25, or 465 for security::tls
        security tls = security::starttls;
        bool verify = true;                         // check the server's certificate (and its name) when using TLS
        std::string ca_file;                        // the CAs to check it against; empty: the system's
        std::string user, password;                 // sign in (AUTH) when user isn't empty
        std::string helo;                           // the name given in EHLO; empty: this host's name
        std::chrono::seconds connect_timeout{30};
        std::chrono::seconds timeout{120};          // the whole conversation, per message
    };

    // Who a message is from and who gets it: addresses only ("ann@example.com"), not "Ann <ann@example.com>"; the
    // message's own From, To and Cc headers are what people see. An empty from is the null sender (<>, bounces).
    struct envelope {
        std::string from;
        std::vector<std::string> to;
    };

    // What went wrong: why() as text, code() the server's last reply code (550, 421, ...) when it answered, else 0.
    class error : public std::runtime_error {
    public:
        error(const std::string &what, int code) : std::runtime_error{what}, code_{code} {}
        [[nodiscard]] int code() const { return code_; }
        // Whether trying again later may work: no answer, or a 4xx reply.
        [[nodiscard]] bool temporary() const { return code_ == 0 || (code_ >= 400 && code_ < 500); }

    private:
        int code_;
    };

    class client {
    public:
        explicit client(settings how);

        // Sends one message, in a connection of its own: every recipient must be taken, or nothing is sent (an
        // error). Its lines may end in "\n" or "\r\n"; they go as "\r\n". Throws sc::smtp::error.
        void send(const envelope &to, std::string_view message) const;

        [[nodiscard]] const settings &how() const { return how_; }

    private:
        settings how_;
    };

    // The address in "Name <address>" (or a bare address, trimmed).
    [[nodiscard]] std::string address_of(std::string_view mailbox);

    // This host's own sender, as sendmail would give it: the user running the program @ /etc/mailname, or the
    // host's name ("root@sync1.example.com").
    [[nodiscard]] std::string default_sender();

    [[nodiscard]] security security_from(std::string_view name); // "none", "starttls", "starttls_required", "tls"
}

#endif
