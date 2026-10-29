const fs = require('node:fs');
require('esbuild').buildSync({entryPoints:['web/videojs-entry.js'],
  outfile:'web/vendor/videojs.min.js', bundle:true, minify:true,
  format:'iife', target:['es2022'], legalComments:'eof'});
fs.copyFileSync('node_modules/@videojs/html/LICENSE', 'web/vendor/videojs.LICENSE');
