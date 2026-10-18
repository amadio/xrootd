add_bats_test(gsi.bats)

if(BUILD_KRB5 AND NOT APPLE)
  add_bats_test(krb5.bats)
endif()

add_bats_test(sss.bats)
