add_bats_test(client-cert.bats)

if(BUILD_MACAROONS)
  add_bats_test(macaroons.bats)
endif()
