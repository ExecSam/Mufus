Mufus website
=============

The one-page site for [mufus.samjackaman.dev](https://mufus.samjackaman.dev): static HTML/CSS/JS in
`public/`, served by an unprivileged nginx container.

Running it
----------

```
cd website
cp .env.example .env    # set PORT, BIND_ADDRESS, CONTAINER_NAME
docker compose up -d --build
```

The site is then on `http://<host>:<PORT>/` (8080 by default). Set `BIND_ADDRESS=127.0.0.1` when a
reverse proxy or Cloudflare Tunnel on the same host sits in front of it.

To update after pulling changes: `docker compose up -d --build`.

Releases
--------

The download links, version, sizes and SHA-256 hashes are read from the latest GitHub release by the
browser (cached for 30 minutes), so new releases show up without touching the site. The values in
`index.html` are only the fallback for when the GitHub API can't be reached, so bump them from time to
time (search for the current version number).

Notes
-----

* `nginx.conf` sets a Content Security Policy that only allows the site's own files and the GitHub API.
  If you add analytics or anything else served from another domain (including Cloudflare Web Analytics
  auto-injection), add it to the policy.
* HTML, CSS and JS are revalidated on every visit, so changes show up straight away. Images are cached
  for a day.
* The screenshots in `public/img/` come from the `ui` end-to-end test (`tests/e2e`).
