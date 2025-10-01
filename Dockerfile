FROM theypsilon/gcc-arm:10.2-2020.11
LABEL maintainer="theypsilon@gmail.com"
WORKDIR /project
ADD . /project
RUN /opt/intelFPGA_lite/quartus/bin/quartus_sh --flow compile make
CMD ["cat", "/project/MiSTer"]
