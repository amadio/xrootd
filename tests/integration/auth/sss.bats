#!/usr/bin/env bash

load ../functions.sh

function setup_sss() {

	export XrdSecPROTOCOL=sss
	export XrdSecSSSKT="${PWD}/sss.keytab"

	xrdsssadmin add "${XrdSecSSSKT}" <<< y
	xrdsssadmin -u xrootd -g xrootd add "${XrdSecSSSKT}"
	xrdsssadmin -u "$(id -un)" -g "$(id -gn)" add "${XrdSecSSSKT}"
	xrdsssadmin list "${XrdSecSSSKT}"
	xrdsssadmin -u xrootd -g xrootd del "${XrdSecSSSKT}"

	mkdir data

	export HOST=${HOSTNAME}:5555
}

@test "We can authenticate with sss" {
	xrdfs "${HOST}" query config version
	grep -c "Authenticated with sss" "${XRD_LOGFILE}"
}

@test "We can copy a file to the server and back" {
	echo "Hello, XRootD!" >| hello.txt
	xrdcp hello.txt "${HOST}"/hello.txt
	xrdcp "${HOST}"/hello.txt hello-downloaded.txt
	diff -u hello.txt hello-downloaded.txt
}

@test "Authentication fails with a bad (insecure) sss keytab" {
	cp "${XrdSecSSSKT}" sss.keytab
	chmod 644 sss.keytab
	run -52 env XrdSecSSSKT=sss.keytab xrdfs "${HOST}" ls /
	grep -c "Auth failed" "${XRD_LOGFILE}"
}

@test "Authentication fails with an invalid sss keytab (/dev/null)" {
	run -52 env XrdSecSSSKT=/dev/null xrdfs "${HOST}" ls /
	assert grep -c "Auth failed" "${XRD_LOGFILE}"
}

@test "Parsing of keytab file with invalid keys fails gracefully" {
	cp "${XrdSecSSSKT}" sss.keytab
	cat >| sss.keytab <<-EOF
	0 u:user1 g:group1 n:name1 N:1 c:1 e:0 f:0 k:
	0 u:user2 g:group2 n:name2 N:2 c:2 e:0 f:0 k:1
	0 u:user3 g:group3 n:name3 N:3 c:3 e:0 f:0 k:x
	0 u:user4 g:group4 n:name4 N:4 c:4 e:0 f:0 k:123456789
	EOF
	test "$(grep -c "keyval has invalid value" <(xrdsssadmin list sss.keytab 2>&1))" == 4
}
