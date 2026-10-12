# Studio Duo website

The public product site lives in `website/`. It explains the accepted mission
and product principles, shows actual application views, and links to releases,
the user guide, and the source. It is static HTML and CSS with a small
JavaScript enhancement for enlarging screenshots. Vite builds the deployable
files; no server, account, analytics, cookies, or external font service is
needed at runtime. Content and image links remain usable without JavaScript.

## Local development

Use Node.js 24 LTS and npm:

```sh
cd website
npm ci
npm run dev
```

Open the local URL printed by Vite, including `/studio-duo/`.
For a production preview:

```sh
npm run build
npm run preview
```

The output is `website/dist/`. The website is independent of the native CMake
build and does not change application packaging.

## Browser checks

```sh
cd website
npm ci
npm run build
npx playwright install chromium
npm test
npm audit --audit-level=low
```

The browser suite starts the production preview server and checks desktop,
tablet, mobile, and 320 px layouts at the real Pages subpath. It covers local
asset loading, navigation, download destinations, screenshot enlargement and
keyboard focus restoration, no-JavaScript fallback, reduced motion, and
automated WCAG AA checks. Generated screenshots and failure traces are kept
under ignored `website/test-results/`. Automated checks supplement, rather
than replace, manual accessibility review.

## GitHub Pages

In the repository's **Settings > Pages**, select **GitHub Actions** as the
build source. The default deployment URL is:

```text
https://mbianchidev.github.io/studio-duo/
```

`.github/workflows/website.yml` installs locked dependencies, audits, builds,
checks the production site, and uploads only `website/dist/`. Pull requests
validate and upload an artifact but cannot deploy. A website change merged
into `main` deploys to the `github-pages` environment; a manual workflow run
on `main` also deploys. A run on another branch only validates. The deploy job
has the Pages and OIDC write permissions; the build job only reads repository
contents. Production deployments queue instead of canceling one another.
The workflow checks the public page after deployment.

`vite.config.js` fixes the base path to `/studio-duo/`. Images use relative
links and the built fonts/scripts/styles respect that path. For a different
repository name or a root-level custom domain, update the Vite base path, the
canonical and Open Graph URLs in `index.html`, and the test server URL in
`playwright.config.js` together. No client-side router or SPA fallback is
required.

The npm script policy explicitly denies the optional macOS `fsevents` install
script. Vite can use its portable file watching without running that native
dependency's installer.

## Content and screenshots

Keep claims aligned with the current [user guide](user-guide.md) and
[accepted product design](design.md). Link to `releases/latest` instead of
hard-coding a version or platform asset filename. Mark native previews as
previews, and retain explicit limits such as ARA compatibility mode,
DAWproject compatibility reports, and the external licensed DDP encoder.

State Windows support as best effort only, with the one-person team's lack of
a Windows testing device and limited testing time. Keep Windows ASIO separate
from macOS CoreAudio: the reported physical-interface test is Focusrite Scarlett
Duo on macOS ARM. macOS release builds target 12.0 and include ARM64/x86_64;
Windows 10/11 x64 are best-effort target platforms, not a claim of validated
minimum hardware performance. Linux is unsupported because of development-time,
audio-driver, and distribution-coverage constraints.

ScreamForge and Track2Stem are optional, separately installed companion
projects from the same developer. ScreamForge is a vocal VST3/AU processor,
not a guitar amp. Its repository is private, so use the requested email contact
instead of a GitHub repository link. Track2Stem uses AI-powered Demucs separation
outside Studio Duo; do not describe it as a bundled native AI feature.

The hero uses the requested "A new DAWn for music production" headline, with
"Your music. Your decisions." below it. Preserve the mixed case in `DAWn`,
the exact core statement, and the visible `2026 SyncTune` footer notice when
changing typography. The screenshot viewer's image has no source while its
dialog is closed; the script assigns the local image URL before showing it.
This is deferred image loading, not a visible broken-image placeholder.

The DAW comparison is a sourced workflow summary, not a performance benchmark
or feature-parity claim. Keep Fender Studio's free recording app separate from
Fender Studio Pro. Scope Ableton to the stated Standard/Suite editions and
Pro Tools to Studio, not Ultimate. Do not invent track limits, pricing,
certifications, or unavailable features from missing documentation.
The table includes Studio Duo's limited test coverage, best-effort Windows,
unsupported Linux, missing AAX hosting, and interchange limits.

Comparison sources checked on 2026-10-12:

- [Fender Studio knowledge base](https://support.presonus.com/hc/en-us/categories/46418271543565-Fender-Studio)
- [Fender Studio Pro overview](https://www.fender.com/pages/fender-studio-pro)
- [Fender Studio Pro licensing FAQ](https://intl.fender.com/pages/fender-studio-pro-faqs)
- [Ableton edition comparison](https://www.ableton.com/en/live/compare-editions/)
- [Pro Tools edition comparison](https://www.avid.com/pro-tools/comparison)
- [Pro Tools MIDI workflows](https://www.avid.com/pro-tools/midi)
- [Avid AAX documentation](https://developer.avid.com/aax/)
- [REAPER overview](https://www.reaper.fm/) and
  [platform/format information](https://www.reaper.fm/about.php)
- [REAPER license information](https://www.reaper.fm/purchase.php)

The committed WebP screenshots are local assets, not runtime hotlinks:

- `session.webp` is the application's Metal Tracking template screenshot
  already published in the repository README:
  `https://github.com/user-attachments/assets/21b07b83-1ca7-4809-9e0d-e17088a1581e`.
- `tracking.webp` and `mixer.webp` are unaltered UI crops from that screenshot,
  not separate sessions or fabricated populated projects.
- `drum-geometry.webp` is the native 1100 px editor preview rendered during the
  drum-geometry implementation, using a synthetic double-kick MIDI pattern.
  Its original rendered size is 2200 x 860 pixels. It contains no customer data.
- `studio-duo-icon.svg` is copied from `assets/branding/`; keep the waveform
  intact when updating the site identity.

Images retain their aspect ratios and descriptive alternative text. The
full-resolution geometry and mixer details are available through the
keyboard-accessible native screenshot dialog; image links work directly
when JavaScript is disabled. Use optimized WebP for new captures, provide
explicit dimensions, and lazy-load images below the hero.

Barlow Condensed and IBM Plex Sans are self-hosted through their Fontsource
packages. Their SIL Open Font License notices ship in `public/font-licenses/`
and are copied into the deployed site. When upgrading the font packages,
refresh those notices from the installed packages. Website npm dependencies
and GitHub Actions are covered by weekly Dependabot updates.
