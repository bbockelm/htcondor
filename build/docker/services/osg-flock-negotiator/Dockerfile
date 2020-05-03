# This is a negotiator for the OSG pool.

ARG EL
ARG VERSION
ARG SUFFIX

FROM htcondor/base

# https://label-schema.org/rc1
LABEL org.label-schema.name="htcondor/cm:${VERSION}-el${EL}${SUFFIX}" \
      org.label-schema.description="HTCondor ${VERSION} central manager image for OSG's pool" \
      org.label-schema.vendor="HTCondor" \
      org.label-schema.license="Apache-2.0"

USER root

COPY osg-flock-negotiator/rpmbuild/*.rpm /root/
COPY osg-flock-negotiator/container-files /

RUN rpm -Uhv https://repo.opensciencegrid.org/osg/3.5/osg-3.5-el7-release-latest.rpm && \
    rpmkeys --import /etc/pki/rpm-gpg/RPM-GPG-KEY-OSG && \
    (rpm -qi gpg-pubkey-824b8603 | gpg --keyid-format 0xlong | grep -q '1024D/0x2110B1C8824B8603') && \
    yum -y localinstall --disablerepo=htcondor-development /root/*.rpm && \
    yum -y install git supervisor && \
    git clone https://github.com/opensciencegrid/osg-flock.git && \
    cp osg-flock/flock.opensciencegrid.org/htcondor/config.d/* /etc/condor/config.d/ && \
    rm -rf osg-flock && \
    yum clean all && rm -rf /tmp/yum*

ENTRYPOINT ["/usr/bin/supervisord", "-c", "/etc/supervisord.conf"]

LABEL org.label-schema.build-date="${BUILDDATE}"
