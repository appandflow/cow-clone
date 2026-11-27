# Releasing

CI builds the six prebuilt binaries (macOS, Linux and Windows, each on x64 and
arm64), assembles them into the npm tarball with `npm run pack:release`, and
uploads it as the `npm-package` artifact of every run. The smoke jobs install
that tarball on every platform and run the CLI and the API.

## First publish

npm trusted publishing cannot create a package, so 0.1.0 is published by
hand from the `npm-package` artifact of a green CI run on `main`:

```sh
gh run download <run-id> --repo appandflow/cow-clone --name npm-package --dir release
npm login
npm publish ./release/cow-clone-0.1.0.tgz --access public
```

The `./` matters: without it npm reads `release/...` as a GitHub repository.
Publish the tarball CI built; a local checkout holds only the binary for the
machine it was built on.

Do not tag `v0.1.0` before the manual publish: the release workflow would try
to publish without a trusted publisher and fail. After it, a `v0.1.0` tag
is safe: the workflow sees the version on npm and stops with a notice.

Then configure the trusted publisher on npmjs.com (package Settings, Trusted
Publisher):

| Field                | Value          |
| -------------------- | -------------- |
| Publisher            | GitHub Actions |
| Organization or user | `appandflow`   |
| Repository           | `cow-clone`    |
| Workflow filename    | `release.yml`  |
| Environment name     | (empty)        |

## Tag-driven releases

1. Bump `version` in `package.json` and `CHANGELOG.md`, and merge it to `main`.
2. Tag the merge commit `v<version>` and push the tag.

The workflow checks that the tag matches the package version and that the
version is not on npm yet; when it already is, it stops there with a notice
and succeeds. Otherwise it runs CI on the tag and publishes the CI-built
tarball with provenance through npm trusted publishing. It needs no npm
token.
