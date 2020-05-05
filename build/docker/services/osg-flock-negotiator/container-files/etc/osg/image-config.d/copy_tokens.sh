#!/bin/sh

if [ $(ls -A /etc/condor/tokens-orig.d/* 2>/dev/null) ]; then
  mkdir -p /etc/condor/tokens-tmp.d
  cp /etc/condor/tokens-orig.d/* /etc/condor/tokens-tmp.d/
  chown -R condor: /etc/condor/tokens-tmp.d
  chmod 0700 /etc/condor/tokens-tmp.d
  chmod 0600 /etc/condor/tokens-tmp.d/*
  mv /etc/condor/tokens-tmp.d /etc/condor/tokens.d
fi
