#!/bin/sh

if [ $(ls -A /etc/condor/tokens-orig.d/* 2>/dev/null) ]; then
  mkdir -p /etc/condor/tokens-tmp.d
  cp /etc/condor/tokens-orig.d/* /etc/condor/tokens-tmp.d/
  chown -R condor: /etc/condor/tokens.d
  chown -R condor: /etc/condor/tokens-tmp.d
  chmod 0700 /etc/condor/tokens.d
  chmod 0600 /etc/condor/tokens-tmp.d/*
  mv /etc/condor/tokens-tmp.d/* /etc/condor/tokens.d
fi

if [ $(ls -A /etc/condor/passwords-orig.d/* 2>/dev/null) ]; then
  mkdir -p /etc/condor/passwords-tmp.d
  cp /etc/condor/passwords-orig.d/* /etc/condor/passwords-tmp.d/
  chown -R root: /etc/condor/passwords.d
  chown -R root: /etc/condor/passwords-tmp.d
  chmod 0700 /etc/condor/passwords.d
  chmod 0600 /etc/condor/passwords-tmp.d/*
  mv /etc/condor/passwords-tmp.d/* /etc/condor/passwords.d
fi
