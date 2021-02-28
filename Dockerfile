FROM theypsilon/gcc-linaro:7.5.0
LABEL maintainer="theypsilon@gmail.com"
WORKDIR /project
ADD . /project
RUN /opt/intelFPGA_lite/quartus/bin/quartus_sh --flow compile make
CMD cat /project/MiSTer
