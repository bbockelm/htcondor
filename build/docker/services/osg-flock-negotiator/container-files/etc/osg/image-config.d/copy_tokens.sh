#!/bin/sh

if [ $(ls -A /etc/condor/tokens-orig.d/* 2>/dev/null) ];
  cp /etc/condor/tokens-orig.d/* /etc/condor/tokens.d/
fi
