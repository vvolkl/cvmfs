PREFIX ?= out/

all: cvmfs_snapshotter

cvmfs_snapshotter: 
	go build -o $(PREFIX)$@ -ldflags "-X main.version=${CernVM-FS_VERSION_STRING}"

release: cvmfs_snapshotter
	mkdir -p release/usr/local/bin
	cp -r out/* release/usr/local/bin
	cp -r script/config/* release/
