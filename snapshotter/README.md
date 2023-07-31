# CVMFS Containerd Snapshotter

**It requires containerd >= 1.4.0-beta.1**

This repository contains a containerd snapshotter that exploits the CernVM-FS to provide the filesystem for the containers.

## Background

From version 1.4.0, containerd introduced the concept of remote snapshotter, a specialized component responsible for assembling all the layers of container images into a stacked filesystem that containerd can use.

We refer users to the official [documentation](https://cvmfs.readthedocs.io/en/latest/cpt-containers.html#containerd-remote-snapshotter-plugin) for information about how to configure and use the CVMFS Containerd Snapshotter.

## Work in progress

This snapshotter is still a work in progress.

Feel free to fill issues and pull requests.

## Testing

This plugin is tested using `kind`.

```
$ docker build -t cvmfs-kind-node https://github.com/marcoverl/cvmfs.git\#:snapshotter
$ cat kind-mount-cvmfs.yaml
kind: Cluster
apiVersion: kind.x-k8s.io/v1alpha4
nodes:
- role: control-plane
  extraMounts:
    - hostPath: /cvmfs/unpacked.cern.ch
      containerPath: /cvmfs/unpacked.cern.ch
- role: worker
  extraMounts:
    - hostPath: /cvmfs/unpacked.cern.ch
      containerPath: /cvmfs/unpacked.cern.ch

$ kind create cluster --config kind-mount-cvmfs.yaml --image cvmfs-kind-node
```

At this point, it is possible to use `kubectl` to start containers. The directory examples contains a recipe to launch a pod running a image used for the tests described in doi:10.3389/fdata.2021.673163.
If the filesystem of the container is available on the local filesystem used by the plugin, it won't download the tarball, but just mount the local filesystem.

### Many thanks

Thanks to @ktock and the containerd community for the work on a similar plugin and API.

[https://github.com/containerd/stargz-snapshotter/](https://github.com/containerd/stargz-snapshotter/)
