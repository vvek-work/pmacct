> brew install autoconf
> 
> brew install libtool
> 
> brew install automake
> 
> brew install pkg-config
> 
> brew install jansson
> 
> brew install zeromq
> 
> brew install ndpi

> ./autogen.sh

> ./configure --disable-bgp-bins --disable-bmp-bins --disable-st-bins --enable-jansson --enable-zmq --enable-threads --enable-64bit --enable-ndpi

> make
 
> sudo make install

> src/pmacctd
> 
> src/nfacctd
> 
> src/sfacctd