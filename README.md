# simply-cpp-smtp

SMTP for simply-cpp: a C++20 client that sends mail to a relay (a smarthost or your mail server) from the program
itself: no `sendmail`, no other program run. [libcurl](https://curl.se/libcurl/) speaks the protocol, STARTTLS, TLS
and AUTH; nothing of it shows in the API.

```cpp
#include <sc/mime/mime.h>   // sc-mime writes the message
#include <sc/smtp/smtp.h>

const auto message = sc::mime::draft{}
    .from({"Reports", "reports@example.com"})
    .to({"Ann Smith", "ann@example.com"})
    .subject("Load report")
    .text("Attached please find your load report.")
    .attach("report.txt", "text/plain", report)
    .str();

const sc::smtp::client smtp{{.host = "smtp.example.com"}};
smtp.send({.from = "reports@example.com", .to = {"ann@example.com"}}, message);
```

## What it does

- **One message, one connection**: `send(envelope, text)` connects, says EHLO, gives the envelope (MAIL FROM, a RCPT
  TO each), sends the text and quits. Every recipient must be taken, or nothing is sent. The text's lines may end in
  `"\n"` (as `sc::mime::draft::str()` writes them) or `"\r\n"`; they go as `"\r\n"`, and a line starting with a dot is
  escaped. An empty sender is the null sender (`<>`).
- **The envelope is addresses only**: `address_of("Ann <ann@example.com>")` gives `ann@example.com`. The message's own
  From, To and Cc are what people see; Bcc recipients are only in the envelope. `default_sender()` is what sendmail
  would use: the user running the program @ `/etc/mailname` (or the host's name).
- **Security** (`settings::tls`): `starttls` (default: STARTTLS when the server offers it, as MTAs do),
  `starttls_required`, `tls` (SMTPS, port 465) or `none`. The certificate and its name are checked (`verify`, against
  the system's CAs or `ca_file`). `user` and `password` sign in (AUTH) when given.
- **Errors** are `sc::smtp::error`: `what()`, `code()` (the server's last reply: 550, 451, ...; 0 when it never
  answered) and `temporary()` (no answer, or a 4xx reply: try again later). A message isn't queued: the caller decides
  when to try again.
- **Time limits**: `connect_timeout` (30 s) and `timeout` (120 s, the whole conversation).
- **Probing an address** (`<sc/smtp/probe.h>`, since 3.1.0): `probe(address, settings)` asks the domain's mail
  servers whether they'd take mail for it, without sending any: EHLO (HELO when EHLO is refused), MAIL FROM, RCPT TO,
  then RSET and QUIT; never DATA. The servers are `mail_exchangers(domain)`: the MX hosts by preference, the domain
  itself when it has none, none for a null MX. A server that can't be reached or doesn't greet with 220 passes the
  question on to the next. The result is the deciding reply (`code`, `reply`, `host`, `stage`: RCPT TO's, or an
  earlier refusal of EHLO or the sender), or code 0 with `problem` saying why no server answered. Plain TCP to port
  25, with `connect_timeout` (per address), `command_timeout` (per reply) and `total_timeout` (10, 20 and 30 s).
  What an answer means is the caller's call: catch-all servers take every address. `host_name()` is the host's
  fully qualified name, as an MTA gives it in EHLO. It uses the system's resolver library (libresolv).

## Building and testing

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

It needs sc-core and libcurl (`libcurl4-openssl-dev`; macOS has it), found or installed by CMake. The test runs a
small SMTP server of its own on 127.0.0.1 and checks what it's told; the probe's test does the same, with servers
that refuse, greylist, greet with 554 or never answer.

`sc-smtp-demo HOST FROM TO [SUBJECT]` sends a short mail.

## License

Apache License 2.0 (see `LICENSE` and `NOTICE`). libcurl is under the curl license.
