#!/bin/bash

progdir=${0%/*}

[[ -e $progdir/env.sh ]] && source "$progdir/env.sh"

set -eu

if [[ $DOCKER_IMAGE = centos* ]]; then
    yum -y install epel-release
fi
sudo yum -y install ${RPM_DEPENDENCIES[@]}
sudo yum -y install python36-devel boost169-devel boost169-static scitokens-cpp-devel

mkdir -p cmake_build
pushd cmake_build
time cmake ${CMAKE_OPTIONS[@]} ..
time make srpm
rpm -Uhv build/packaging/srpm/condor-*.src.rpm
rpmbuild -D 'osg 1' -ba ~/rpmbuild/SPECS/condor.spec

# vim:et:sw=4:sts=4:ts=8
