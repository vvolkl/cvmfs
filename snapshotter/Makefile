PREFIX ?= out/

all: cvmfs_snapshotter

cvmfs_snapshotter: 
	go build -o $(PREFIX)$@


release: cvmfs_snapshotter
	mkdir -p release/usr/local/bin
	cp -r out/* release/usr/local/bin
	cp -r script/config/* release/
