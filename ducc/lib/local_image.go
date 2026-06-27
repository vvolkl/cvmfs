package lib

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"strings"
	"sync"

	"github.com/google/go-containerregistry/pkg/name"
	v1 "github.com/google/go-containerregistry/pkg/v1"
	"github.com/google/go-containerregistry/pkg/v1/layout"
	"github.com/google/go-containerregistry/pkg/v1/tarball"

	da "github.com/cvmfs/ducc/docker-api"
)

// Local images (loaded from a tarball or an OCI layout) are kept in a
// process-global registry keyed by their canonical name. CreateWish and the
// wildcard expansion turn an Image back into a string (WholeName) and re-parse
// it, which would otherwise drop the loaded image and fall back to HTTP. By
// re-attaching from this registry inside ParseImage, every round-trip of the
// same name transparently keeps reading from the local source.
var (
	localImagesMu sync.Mutex
	localImages   = make(map[string]v1.Image)
)

func registerLocalImage(key string, img v1.Image) {
	localImagesMu.Lock()
	defer localImagesMu.Unlock()
	localImages[key] = img
}

func lookupLocalImage(key string) v1.Image {
	localImagesMu.Lock()
	defer localImagesMu.Unlock()
	return localImages[key]
}

// localKey is the canonical name under which a local image is registered.
func (i Image) localKey() string {
	return i.Registry + "/" + i.Repository + ":" + i.Tag
}

// ParseImage parses an image reference. In addition to registry references it
// understands skopeo-style local transports, so an image can be converted from
// the local filesystem without (re-)pulling it from a registry -- useful for
// testing and air-gapped conversion:
//
//	docker-archive:<path>   a tarball produced by `docker save` / `podman save`
//	oci:<path>              an OCI image layout directory
//
// For a local reference the returned Image carries a go-containerregistry image;
// the manifest, config and layer fetchers read from it instead of issuing HTTP
// requests (see the localImage branches in image.go).
func ParseImage(image string) (Image, error) {
	if img, ok, err := parseLocalImageRef(image); ok {
		return img, err
	}
	img, err := parseImageRemote(image)
	if err != nil {
		return img, err
	}
	if li := lookupLocalImage(img.localKey()); li != nil {
		img.localImage = li
	}
	return img, nil
}

func parseLocalImageRef(image string) (Image, bool, error) {
	var transport, path string
	switch {
	case strings.HasPrefix(image, "docker-archive:"):
		transport, path = "docker-archive", strings.TrimPrefix(image, "docker-archive:")
	case strings.HasPrefix(image, "oci:"):
		transport, path = "oci", strings.TrimPrefix(image, "oci:")
	default:
		return Image{}, false, nil
	}
	if path == "" {
		return Image{}, true, fmt.Errorf("missing path in local image reference %q", image)
	}

	var (
		img     v1.Image
		repoTag string
		err     error
	)
	switch transport {
	case "docker-archive":
		img, repoTag, err = loadDockerArchive(path)
	case "oci":
		img, repoTag, err = loadOCILayout(path)
	}
	if err != nil {
		return Image{}, true, fmt.Errorf("loading %s image from %q: %w", transport, path, err)
	}

	result, err := newLocalImage(img, repoTag)
	if err != nil {
		return Image{}, true, err
	}
	registerLocalImage(result.localKey(), img)
	return result, true, nil
}

func loadDockerArchive(path string) (v1.Image, string, error) {
	img, err := tarball.ImageFromPath(path, nil)
	if err != nil {
		return nil, "", err
	}
	// Recover the image's name from the archive's RepoTags, if present.
	repoTag := ""
	if manifest, mErr := tarball.LoadManifest(func() (io.ReadCloser, error) {
		return os.Open(path)
	}); mErr == nil {
		for _, descriptor := range manifest {
			if len(descriptor.RepoTags) > 0 {
				repoTag = descriptor.RepoTags[0]
				break
			}
		}
	}
	return img, repoTag, nil
}

func loadOCILayout(path string) (v1.Image, string, error) {
	index, err := layout.ImageIndexFromPath(path)
	if err != nil {
		return nil, "", err
	}
	manifest, err := index.IndexManifest()
	if err != nil {
		return nil, "", err
	}
	for _, descriptor := range manifest.Manifests {
		if !descriptor.MediaType.IsImage() {
			continue
		}
		img, err := index.Image(descriptor.Digest)
		if err != nil {
			return nil, "", err
		}
		repoTag := ""
		if descriptor.Annotations != nil {
			repoTag = descriptor.Annotations["org.opencontainers.image.ref.name"]
		}
		return img, repoTag, nil
	}
	return nil, "", fmt.Errorf("no image manifest found in OCI layout %q", path)
}

// newLocalImage builds a ducc Image whose identity (registry/repository/tag) is
// derived from repoTag and whose payloads are served from the loaded image.
func newLocalImage(img v1.Image, repoTag string) (Image, error) {
	if repoTag == "" {
		repoTag = "localhost/local-image:latest"
	}
	ref, err := name.ParseReference(repoTag, name.WeakValidation)
	if err != nil {
		return Image{}, fmt.Errorf("parsing image name %q: %w", repoTag, err)
	}
	tag := "latest"
	if tagged, ok := ref.(name.Tag); ok {
		tag = tagged.TagStr()
	}
	return Image{
		Scheme:     "https",
		Registry:   ref.Context().RegistryStr(),
		Repository: ref.Context().RepositoryStr(),
		Tag:        tag,
		localImage: img,
	}, nil
}

// localManifestListBytes synthesises a single-entry manifest list pointing at
// the local image's manifest, matching what FetchManifestList2 expects.
func (img *Image) localManifestListBytes() ([]byte, error) {
	digest, err := img.localImage.Digest()
	if err != nil {
		return nil, err
	}
	mediaType, err := img.localImage.MediaType()
	if err != nil {
		return nil, err
	}
	list := da.ManifestList{
		SchemaVersion: 2,
		MediaType:     string(mediaType),
		Manifests: []da.ManifestListItem{{
			MediaType: string(mediaType),
			Digest:    digest.String(),
		}},
	}
	return json.Marshal(list)
}

// localLayer returns the (uncompressed) tar stream of a layer from the local
// image, matching the contract of downloadLayer (which gunzips remote blobs).
func (img *Image) localLayer(layer da.Layer) (downloadedLayer, error) {
	hash, err := v1.NewHash(layer.Digest)
	if err != nil {
		return downloadedLayer{}, err
	}
	l, err := img.localImage.LayerByDigest(hash)
	if err != nil {
		return downloadedLayer{}, err
	}
	reader, err := l.Uncompressed()
	if err != nil {
		return downloadedLayer{}, err
	}
	return newDownloadedLayer(layer.Digest, NewReadAndHash(reader)), nil
}
