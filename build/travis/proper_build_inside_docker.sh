#!/bin/bash

progdir=${0%/*}

[[ -e $progdir/env.sh ]] && source "$progdir/env.sh"

set -eu

if [[ $DOCKER_IMAGE = centos* ]]; then
    yum -y install epel-release
fi
sudo yum -y install ${RPM_DEPENDENCIES[@]}
sudo yum -y install python36-devel boost169-devel boost169-static scitokens-cpp-devel

mkdir -p $HOME/cmake_build
pushd $HOME/cmake_build
time cmake ${CMAKE_OPTIONS[@]} $OLDPWD
time make srpm
rpm -Uhv build/packaging/srpm/condor-*.src.rpm
rpmbuild -D 'noblahp 1' -D 'osg 1' -ba ~/rpmbuild/SPECS/condor.spec

# vim:et:sw=4:sts=4:ts=8
