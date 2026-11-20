#ifndef SC_SMTP_PROBE_H
#define SC_SMTP_PROBE_H
// Asking a domain's mail servers whether they would take mail for an address, without sending any: the start of an
// SMTP conversation (RFC 5321: EHLO, MAIL FROM, RCPT TO), then RSET and QUIT. DATA is never sent. The servers are the
// domain's MX hosts by preference, or the domain itself when it has none (RFC 5321 5.1); a server that can't be
// reached, or doesn't greet with 220, passes the question on to the next. Plain TCP to port 25, no TLS: a probe
// carries no message.
//
//     const auto answer = sc::smtp::probe("ann@example.com", {.from = "checks@example.net"});
//     if (answer.code == 250) ...       // taken
//
// Whether an answer means the address exists is the caller's call: 250/251 take it, 5xx refuse it, 4xx (greylisting,
// a full mailbox, a busy server) say nothing for sure. Servers that take every address (catch-alls), and ones that
// only refuse after DATA, can't be told apart from real mailboxes this way.

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace sc::smtp {
    struct probe_settings {
        std::string from;                                 // MAIL FROM; empty: the null sender <>
        std::string helo;                                 // the name given in EHLO; empty: host_name()
        int port = 25;
        std::chrono::milliseconds connect_timeout{10000}; // per address tried
        std::chrono::milliseconds command_timeout{20000}; // per reply waited for
        std::chrono::milliseconds total_timeout{30000};   // the whole probe, every server tried
    };

    // Where a probe ended: none before any server was tried, connect when no server greeted it (problem says why),
    // else the step whose reply decided it.
    enum class probe_stage { none, connect, ehlo, mail, rcpt };

    struct probe_result {
        // The deciding reply's code: RCPT TO's, or the EHLO or MAIL FROM refusal that came before it; 0 when no server
        // answered (stage connect), or one stopped answering.
        int code = 0;
        std::string reply;                    // that reply, its lines joined by "\n" ("550 5.1.1 No such user")
        std::string host;                     // the server that gave it
        probe_stage stage = probe_stage::none;
        // Why no server answered, or why a conversation broke off, each server tried in turn
        // ("mx1.example.com: Connection refused; mx2.example.com: timed out")
        std::string problem;
    };

    // The domain's mail servers (MX records) by preference, the most preferred first; the domain itself when it has
    // none (no MX records, or the domain doesn't resolve: connecting then says why); empty for a null MX ("." , RFC
    // 7505: the domain takes no mail). A trailing dot is dropped.
    [[nodiscard]] std::vector<std::string> mail_exchangers(std::string_view domain);

    // Probes address (local@domain, already checked: it goes into the commands as given) at hosts, or at the domain's
    // mail_exchangers() when hosts is empty. Never throws for what the network or the servers do; their answers and
    // failures are in the result.
    [[nodiscard]] probe_result probe(std::string_view address, const probe_settings &how, std::vector<std::string> hosts = {});

    // This host's fully qualified name, as an MTA gives it in EHLO: the canonical name of gethostname() (resolved),
    // or gethostname() itself.
    [[nodiscard]] std::string host_name();

    [[nodiscard]] std::string_view stage_name(probe_stage stage); // "none", "connect", "ehlo", "mail", "rcpt"
}

#endif
