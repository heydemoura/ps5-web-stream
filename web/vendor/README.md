# Browser player dependency

`flv.min.js` is the unmodified distribution from `flv.js` **1.6.2**,
[bilibili/flv.js](https://github.com/bilibili/flv.js), licensed under Apache-2.0
(see `flv.LICENSE`). It is embedded in the payload and served from the console;
preview playback does not require access to a CDN or the internet.

Source package: https://registry.npmjs.org/flv.js/-/flv.js-1.6.2.tgz

The downloaded package was verified against the npm registry's SHA-512 integrity:

```
sha512-xre4gUbX1MPtgQRKj2pxJENp/RnaHaxYvy3YToVVCrSmAWUu85b9mug6pTXF6zakUjNP2lFWZ1rkSX7gxhB/2A==
```

SHA-256 of the vendored JavaScript:

```
733b9b325dbc59871a652c0a84f2f285a2cfd06cf2efcedcd87cb1e194cd1e8f
```

## Video.js

`videojs.min.js` bundles the HTML custom-element primitives from
`@videojs/html` **10.2.0**, using esbuild **0.28.2**. Entry point:
`web/videojs-entry.js`; exact dependency integrity is in `package-lock.json`.
Rebuild with `npm ci && npm run build:player`. The Apache-2.0 license is in
`videojs.LICENSE`; bundled dependency notices are retained in the JavaScript.
The components own playback, volume, fullscreen and picture-in-picture; flv.js
continues to deliver H.264/AAC to the same native video element.

Official documentation: https://videojs.org/docs/guides/installation/html
