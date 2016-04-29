FROM centos:6

# copied from builder script
RUN yum -y update && \
    curl -o /etc/yum.repos.d/devtools-2.repo http://people.centos.org/tru/devtools-2/devtools-2.repo && \
    rpm -i http://mirror.pnl.gov/epel/6/i386/epel-release-6-8.noarch.rpm && \
    sed -e 's,$basearch,i386,' -e 's,$releasever\],$releasever-i686\],' /etc/yum.repos.d/devtools-2.repo > /etc/yum.repos.d/devtools-2-i686.repo && \
    yum -y install \
        createrepo \
        devtoolset-2-toolchain \
        dpkg \
        dpkg-devel \
        expect \
        gcc \
        gcc-c++ \
        git \
        glibc-static \
        make \
        rpm-build \
        unzip \
        wget \
        tar && \
     yum -y install \
        glibc-devel.i686 \
        devtoolset-2-libstdc++-devel.i686 && yum clean all
RUN curl -o docker.tgz https://get.docker.com/builds/Linux/x86_64/docker-1.11.0.tgz && \
    tar xfz docker.tgz docker/docker && \
    mv docker/docker /usr/local/bin/docker && \
    chmod +x /usr/local/bin/docker && \
    rm -fr docker.tgz docker/

RUN mkdir -p /code/agent
ADD bootstrap-agent /code/agent/
ADD patches /code/agent/patches

RUN cd /code/agent && ONLY_DEPS=true scl enable devtoolset-2 ./bootstrap-agent && rm -fr dependencies/*.tar* dependencies/*.zip
ADD docker-builder-entrypoint.sh /
VOLUME [ "/code/agent/build", "/out", "/root/.cache" ]
ENTRYPOINT [ "/docker-builder-entrypoint.sh" ]
