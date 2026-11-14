set(SC_TEST_LINK_LIBRARIES sc-smtp)

# Against a small SMTP server the test runs itself on 127.0.0.1.
add_sc_test(smtp LABELS unit TIMEOUT 60)
