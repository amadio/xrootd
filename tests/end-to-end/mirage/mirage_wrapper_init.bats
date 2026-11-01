#!/usr/bin/env bash

# The configuration check of the stacked Mirage OSS, with a server for each
# test.

load ../setup/server.bash

teardown() {
    stop_servers
}

@test "stacked plugin without a path fails to initialize" {
    run -1 start_server mirage_wrapper_nopath.cfg

    assert_output --partial 'no mirage path given in the plugin parameters'
}

@test "stacked plugin with the root path fails to initialize" {
    run -1 start_server mirage_wrapper_root.cfg

    assert_output --partial "mirage path '/' is not allowed"
}

@test "stacked plugin with a valid path initializes" {
    run -0 start_server mirage_wrapper.cfg
}
