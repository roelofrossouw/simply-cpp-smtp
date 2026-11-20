set(SC_TEST_LINK_LIBRARIES sc-smtp)

# Against small SMTP servers the tests run themselves on 127.0.0.1.
add_sc_test(smtp LABELS unit TIMEOUT 60)
add_sc_test(probe LABELS unit TIMEOUT 60)
