#!/bin/sh

[ -d srt-1.2.2 ] || {
	wget https://github.com/Haivision/srt/archive/v1.2.2.tar.gz || { 
		echo "can't download SRT."; 
		exit 1;
	}
	tar -xvf v1.2.2.tar.gz
}

cd srt-1.2.2 && {
	./configure --prefix=../srt
	make && make install
} || {
	echo "missing SRT sources"
}
