

```
cat /etc/cvmfs/default.local
CVMFS_HTTP_PROXY=127.0.0.1:6081
```

# install vmod_dynamic
```
sudo apt-get install libgetdns-dev
git clone https://github.com/nigoroll/libvmod-dynamic
cd libvmod-dynamic/
ls
git checkout 7.1

sudo apt-get install libvarnishapi-dev
./autogen.sh
./configure

sudo apt -y install python3-docutils
./configure
make
sudo make install
```


TODO: investigate ways of preloading the cache
