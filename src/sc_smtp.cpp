#include <sc/smtp/sc_smtp.h>

#include <curl/curl.h>

#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>

#include <pwd.h>
#include <unistd.h>

namespace sc::smtp {
    namespace {
        void global_init() {
            static std::once_flag once;
            std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
        }

        std::string trim(std::string_view text) {
            const auto first = text.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos) return {};
            const auto last = text.find_last_not_of(" \t\r\n");
            return std::string{text.substr(first, last - first + 1)};
        }

        // An address as MAIL FROM/RCPT TO take it, in angle brackets; refused when it could break the command
        std::string bracketed(const std::string &address, const char *what) {
            if (address.find_first_of("<>\r\n \t") != std::string::npos) throw error{std::string{what} + " isn't an address: \"" + address + "\"", 0};
            return "<" + address + ">";
        }

        std::string with_crlf(std::string_view message) {
            std::string out;
            out.reserve(message.size() + message.size() / 40);
            for (std::size_t at = 0; at < message.size(); ++at) {
                if (message[at] == '\n' && (at == 0 || message[at - 1] != '\r')) out += '\r';
                out += message[at];
            }
            if (out.size() < 2 || out.compare(out.size() - 2, 2, "\r\n") != 0) out += "\r\n";
            return out;
        }

        struct reading {
            const std::string *text;
            std::size_t at = 0;
        };

        std::size_t read_part(char *buffer, const std::size_t size, const std::size_t count, void *user) {
            auto &from = *static_cast<reading *>(user);
            const auto n = std::min(size * count, from.text->size() - from.at);
            std::memcpy(buffer, from.text->data() + from.at, n);
            from.at += n;
            return n;
        }

        struct curl_deleter {
            void operator()(CURL *curl) const { curl_easy_cleanup(curl); }
        };
        struct list_deleter {
            void operator()(curl_slist *list) const { curl_slist_free_all(list); }
        };
    }

    security security_from(const std::string_view name) {
        if (name == "none") return security::none;
        if (name == "starttls") return security::starttls;
        if (name == "starttls_required") return security::starttls_required;
        if (name == "tls") return security::tls;
        throw std::invalid_argument{"not an SMTP security: \"" + std::string{name} + "\" (none, starttls, starttls_required or tls)"};
    }

    std::string address_of(const std::string_view mailbox) {
        const auto open = mailbox.rfind('<');
        const auto close = mailbox.rfind('>');
        if (open != std::string_view::npos && close != std::string_view::npos && close > open) return trim(mailbox.substr(open + 1, close - open - 1));
        return trim(mailbox);
    }

    std::string default_sender() {
        std::string user = "root";
        if (const passwd *who = getpwuid(geteuid()); who && who->pw_name) user = who->pw_name;
        std::string host;
        if (std::ifstream mailname{"/etc/mailname"}; mailname) std::getline(mailname, host);
        host = trim(host);
        if (host.empty()) {
            char name[256] = {};
            if (gethostname(name, sizeof name - 1) == 0) host = name;
        }
        return host.empty() ? user : user + "@" + host;
    }

    client::client(settings how) : how_{std::move(how)} {
        if (how_.host.empty()) throw std::invalid_argument{"sc::smtp: no host"};
        if (how_.port == 0) how_.port = how_.tls == security::tls ? 465 : 25;
        global_init();
    }

    void client::send(const envelope &to, const std::string_view message) const {
        if (to.to.empty()) throw error{"no recipients", 0};
        const std::unique_ptr<CURL, curl_deleter> curl{curl_easy_init()};
        if (!curl) throw error{"can't start libcurl", 0};
        CURL *const c = curl.get();

        const auto url = std::string{how_.tls == security::tls ? "smtps://" : "smtp://"}
                         + (how_.host.find(':') != std::string::npos ? "[" + how_.host + "]" : how_.host) + ":" + std::to_string(how_.port)
                         + (how_.helo.empty() ? "" : "/" + how_.helo);
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, static_cast<long>(how_.connect_timeout.count()));
        curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(how_.timeout.count()));
        switch (how_.tls) {
            case security::none: curl_easy_setopt(c, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_NONE)); break;
            case security::starttls: curl_easy_setopt(c, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_TRY)); break;
            case security::starttls_required:
            case security::tls: curl_easy_setopt(c, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL)); break;
        }
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, how_.verify ? 1L : 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, how_.verify ? 2L : 0L);
        if (!how_.ca_file.empty()) curl_easy_setopt(c, CURLOPT_CAINFO, how_.ca_file.c_str());
        if (!how_.user.empty()) {
            curl_easy_setopt(c, CURLOPT_USERNAME, how_.user.c_str());
            curl_easy_setopt(c, CURLOPT_PASSWORD, how_.password.c_str());
        }

        const auto from = to.from.empty() ? std::string{"<>"} : bracketed(to.from, "the sender");
        curl_easy_setopt(c, CURLOPT_MAIL_FROM, from.c_str());
        std::unique_ptr<curl_slist, list_deleter> recipients;
        for (const auto &who: to.to) {
            const auto one = bracketed(who, "a recipient");
            curl_slist *more = curl_slist_append(recipients.get(), one.c_str());
            if (!more) throw error{"out of memory", 0};
            (void) recipients.release();
            recipients.reset(more);
        }
        curl_easy_setopt(c, CURLOPT_MAIL_RCPT, recipients.get());

        const auto text = with_crlf(message);
        reading source{&text};
        curl_easy_setopt(c, CURLOPT_READFUNCTION, read_part);
        curl_easy_setopt(c, CURLOPT_READDATA, &source);
        curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(text.size()));

        char problem[CURL_ERROR_SIZE] = {};
        curl_easy_setopt(c, CURLOPT_ERRORBUFFER, problem);
        if (const CURLcode result = curl_easy_perform(c); result != CURLE_OK) {
            long code = 0;
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
            std::string why = problem[0] ? problem : curl_easy_strerror(result);
            while (!why.empty() && (why.back() == '\n' || why.back() == '\r')) why.pop_back();
            throw error{"SMTP " + how_.host + ":" + std::to_string(how_.port) + ": " + why, static_cast<int>(code)};
        }
    }
}
