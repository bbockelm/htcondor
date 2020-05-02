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

COPY osg-flock/rpmbuild/*.rpm /root/

RUN yum localinstall /root/*.rpm && \
    git clone https://github.com/opensciencegrid/osg-flock.git && \
    cp flock.opensciencegrid.org/htcondor/config.d/* /etc/condor/config.d/

EXPOSE 9618

LABEL org.label-schema.build-date="${BUILDDATE}"
