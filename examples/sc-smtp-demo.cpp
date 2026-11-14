// sc-smtp's example: sends a short mail through an SMTP server.
//
//     sc-smtp-demo HOST FROM TO [SUBJECT]
#include <sc_smtp.h>

#include <params.h>

#include <iostream>

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"description", "Sends a short mail through an SMTP server (STARTTLS when it offers it)."},
        {"positional", {
            {"host", {{"help", "the SMTP server (a relay or smarthost)"}}},
            {"from", {{"help", "the sender's address"}}},
            {"to", {{"help", "the recipient's address"}}},
            {"subject", {{"default", "sc-smtp test"}, {"help", "the subject"}}}}}}};

    const std::string from = args["from"], to = args["to"], subject = args["subject"];
    const sc::smtp::client smtp{{.host = args["host"].get<std::string>()}};
    try {
        smtp.send({.from = from, .to = {to}}, "From: " + from + "\nTo: " + to + "\nSubject: " + subject + "\n\nSent by sc-smtp-demo.\n");
        std::cout << "Sent to " << to << "\n";
    } catch (const sc::smtp::error &error) {
        std::cerr << error.what() << (error.temporary() ? " (try again later)" : "") << "\n";
        return 1;
    }
}
