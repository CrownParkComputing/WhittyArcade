# Google Play paid-distribution feasibility

Assessment date: 21 July 2026. Code baseline: private commit `302bc8a` plus
the Preview 2 release baseline.

This is an engineering and store-policy readiness assessment, not a legal
opinion. A solicitor familiar with software copyright, trade marks and UK/EU
consumer law must approve the rights and end-user terms before a paid public
submission.

## Decision

**Conditional go as an independently branded emulator; no-go as-is.**

WhittyArcade's original code can remain private and proprietary while the
Android application is sold through Google Play. The current permissive
BSD/MIT/zlib components do not inherently prevent commercial distribution.
Google Play's current policies do not state a blanket ban on emulators, but an
app that provides access to third-party games without installation may receive
additional review and every displayed experience remains subject to Play
policy.

Submission is blocked until all of the following are true:

1. every compiled third-party source has recorded provenance and a
   commercial-distribution decision;
2. the non-standard SoftFloat 2b and planned LGPL Android dependencies are
   removed, replaced, or covered by a solicitor-approved compliance package;
3. the Play EULA/addendum is compatible with Google's distribution agreement
   and mandatory consumer rights;
4. store branding, screenshots, compatibility claims and in-app use of game
   names have received an IP review;
5. the Android ARM64 build, original no-ROM review demo, privacy materials and
   Play Console declarations pass their acceptance gates.

The safest product is **WhittyArcade**, using only CrownParkComputing artwork
in the listing, importing only user-selected local files, and making it
unmistakable before purchase that no games or firmware are included. Marketing
it as a paid "Ridge Racer" or "Time Crisis" product, or using their logos,
characters, attract screens, audio or gameplay screenshots, is a no-go without
written rights from the relevant owner.

## Current readiness snapshot

| Gate | Current state | Decision |
|---|---|---|
| Original WhittyArcade code | Private repository and proprietary licence; one Git author identity | Promising, but document ownership of the initial import and the legal owner/trading entity |
| ROM/firmware inclusion | None in source or release packages | Pass; this must remain invariant |
| App networking/analytics/ads | No HTTP client, socket integration, analytics, ads or account system in the native application | Pass for the current tree; re-audit every Android SDK |
| Permissive compiled components | Musashi, Moira core, chips Z80, ymfm and individually marked BSD MAME adaptations | Conditional pass after provenance/SBOM audit and notices |
| GPL material in vendor trees | General MAME GPL text and Moira GPL test-runner files exist but are not in the current link closure | Must be excluded automatically from every commercial build |
| SoftFloat 2b | Compiled into Musashi; unusual commercial/indemnity wording | Blocker pending removal or specific legal approval |
| Android OpenAL/mpg123 plan | Both upstream libraries are LGPL | Blocker pending replacement or complete LGPL relink/source compliance |
| Play-compatible EULA | Current grant is revocable and permits one backup | Blocker; Play's distribution terms take priority where they conflict |
| Game names/marks/screens | Exact Namco/Sega board and game names appear in the catalog/help | Blocker pending IP review; store assets must be original |
| Android product | Scoped but not built | Blocker |
| Reviewer-visible function without ROMs | Launcher/tools exist, but reviewers cannot legally be supplied commercial ROMs | Add an original diagnostic/demo mode |
| Privacy/Data safety | Favourable local-only architecture; no published app privacy policy yet | Add policy URL, in-app page and Console declaration |
| Play merchant/account setup | Not established by this code audit | Verify entity, identity, payments profile, bank and public support details |

## Code and licence analysis

### Original materials

[`LICENSE`](../LICENSE) correctly separates original WhittyArcade materials
from third-party code and states that no game, firmware or trade-mark licence
is granted. As copyright owner, Jonathan Whittingham/CrownParkComputing can
charge for official copies while granting purchasers personal use.

Before sale, establish exactly who contracts with Google and owns the original
copyright:

- if CrownParkComputing is a company, record an assignment of all original
  code and artwork to that company;
- if it is a trading name, describe the legal person consistently as
  "Jonathan Whittingham trading as CrownParkComputing" after legal advice;
- obtain assignments from any other contributor or contractor;
- retain design notes and source/provenance records for the large initial Git
  import, whose earlier authorship history is not visible in this repository;
- clear and, if worthwhile, register the WhittyArcade/CrownParkComputing marks
  before investing in a global listing.

Google does not require the proprietary source to be published. An AAB/APK
does expose native machine code that can be extracted and analysed, so
confidentiality must not be the only protection for a commercially important
algorithm.

### Current compiled third-party closure

The current `compile_commands.json` and CMake target graph compile these
third-party families into the executable:

| Component | Licence evidence | Paid-distribution position | Required action |
|---|---|---|---|
| MAME-derived i960, V60, M37710, MB86233, Model 1, MultiPCM and System 22 adaptations | Relevant imported files identify `BSD-3-Clause` and copyright holders | BSD permits proprietary commercial binaries with attribution | Record upstream URL, exact commit, original path, file licence and local modifications for every import; reproduce exact notices in binary materials |
| MAME project generally | MAME as a whole is GPL-2.0-or-later, while individual files may have permissive headers | Do not rely on "most MAME is BSD"; only the exact individually BSD files may enter the proprietary build | Add a build-closure licence allowlist and fail CI if an unapproved/GPL file is compiled |
| `third_party/mb86233` | Source headers say BSD-3-Clause; the directory also carries MAME's general GPL licence document | Likely usable under each file's express BSD header, but the mixed directory is poor commercial evidence | Preserve the exact upstream file licences separately and have the conclusion checked |
| Moira CPU core | MIT licence in `third_party/moira/Moira/LICENSE` | Commercial/proprietary use allowed with notice | Package the MIT notice; exclude the vendored GPL `Runner` and Binutils files from product and SBOM closure |
| Musashi | Permissive licence reproduced in `THIRD_PARTY_NOTICES.md` | Commercial/proprietary use allowed with notice | Record upstream revision and include notice |
| Berkeley SoftFloat Release 2b | Non-standard notice in the compiled Musashi tree | Text permits commercial derivatives but imposes source notices and unusually broad responsibility/indemnity language | Preferred: prove System 22 does not require the FPU path and remove SoftFloat from the commercial build; otherwise obtain written legal approval |
| chips Z80 | zlib/libpng licence | Commercial/proprietary use allowed | Retain notice and mark modifications |
| ymfm | BSD-3-Clause | Commercial/proprietary use allowed | Retain Aaron Giles notice and full BSD terms |
| SegaPCM, MultiPCM and other adapted sound/video code | Individually identified BSD-3-Clause sources | Commercial/proprietary use is possible | Complete per-file provenance and exact copyright notices |
| SDL3, SDL3_ttf, zlib, MiniZip, GLM and Android toolchain runtime | Release versions are not yet pinned for Android | Expected to be compatible, subject to the exact selected version and transitive dependencies | Lock versions/hashes; include SDL/zlib/MiniZip/GLM, FreeType/HarfBuzz and runtime notices as applicable |
| Bundled font | Not yet selected | Unknown until selected | Use a commercially redistributable font such as an appropriately licensed OFL font; include its licence and reserved-name obligations |

MAME's official licence documentation confirms that the project as a whole is
GPL-2.0-or-later but many individual files use BSD-3-Clause. It also states
that game image files remain copyrighted and discourages advertising or
linking illegal ROM sources:
[MAME licence](https://docs.mamedev.org/license.html) and
[MAME media guidance](https://docs.mamedev.org/whatis.html).

### Planned Android LGPL dependencies

The Linux release dynamically uses system OpenAL and libmpg123 and does not
ship them. The proposed Android package changes that position because it must
bring its own native dependencies:

- OpenAL Soft describes itself as LGPL licensed:
  [upstream](https://github.com/kcat/openal-soft).
- libmpg123 is LGPL 2.1:
  [upstream](https://www.mpg123.de/).

LGPL does not automatically require publishing the original WhittyArcade
engine. It does require preserving the user's applicable rights to the LGPL
library, notices/source and, depending on how it is combined, modification and
relinking. Android app signing and AAB delivery make a casual static-link
approach inappropriate for a closed-source commercial release.

Preferred commercial profile:

1. replace OpenAL output with the existing board-neutral audio abstraction
   backed by SDL audio;
2. replace libmpg123 with a proven permissively licensed MPEG audio decoder;
3. regression-test Model 1 DSB and every board's timing/audio output;
4. keep any remaining third-party library as a separate native target with
   generated notices and source-offer/relink materials where its licence
   requires them.

Retaining LGPL libraries is a viable alternative only after legal review and
an automated compliance bundle. It is a compliance choice, not a technical
shortcut.

### Required commercial SBOM/provenance record

Create one machine-readable manifest per release containing:

- component and version/commit;
- upstream repository and original source path;
- SPDX identifier and full licence text;
- original copyright holders;
- local modifications;
- whether it is compiled, dynamically packaged, build-only or excluded;
- SHA-256 of each staged third-party binary;
- source/relink location for reciprocal-licence components.

CI must generate the actual Android link/package closure and compare it with
an allowlist. A licence scanner is supporting evidence, not a substitute for
the per-file review of adapted MAME code.

## Game, ROM and trade-mark risk

### What the software licences do not grant

Permission to use BSD emulator implementation code does not grant permission
to distribute or market:

- Ridge Racer, Time Crisis, Sega Rally, Virtua Fighter, Star Wars Arcade,
  Shinobi or other game ROMs/firmware;
- game logos, characters, screenshots, music, speech or attract-mode video;
- Namco, Bandai Namco, Sega, Lucasfilm/Star Wars or other names/marks beyond
  uses permitted by applicable trade-mark law;
- arcade manuals, cabinet art or promotional copy;
- a representation that WhittyArcade is official, endorsed or licensed.

ROM filenames, checksums and manifest metadata copied from upstream drivers
also need the same provenance record as code. A disclaimer is useful to avoid
confusion but does not create missing copyright or trade-mark permission.

### Recommended listing route

Use:

- product title `WhittyArcade`;
- original CrownParkComputing icon, colours, crosshairs and diagnostic art;
- original launcher/diagnostic screenshots containing no game frame, logo,
  character, music or publisher artwork;
- a concise pre-purchase statement such as: "Independent arcade-hardware
  emulator. No games or firmware are included. The app does not download game
  files. Users must provide compatible data they are legally entitled to
  use." This wording still requires legal/store review;
- an independent/non-endorsement statement;
- no links, search terms, instructions or UI leading to ROM download sites;
- no use of `MAME` in the app title, icon or promotional branding. Required
  attribution can remain in the Licences page.

Avoid without written publisher permission:

- `Ridge Racer Emulator`, `Time Crisis for Android`, or similar titles;
- "fully working Model 2/Time Crisis" as the headline sales claim;
- official game/publisher logos or gameplay screenshots;
- store keyword stuffing with supported game names;
- videos recorded from commercial ROMs;
- any bundled demo derived from a commercial game's code or assets.

The exact game and board names currently visible in `arcade_catalog.cpp`, the
"Required MAME sets" menu and help text need a solicitor's nominative-use
review. The safest engineering option is a Play-facing generic archive/help
vocabulary while retaining factual internal short names, but code changes do
not replace the legal decision.

Google's current
[Intellectual Property policy](https://support.google.com/googleplay/android-developer/answer/9888072?hl=en)
requires rights for app and listing content, prohibits inducing infringement,
and permits advance contact with written authorisation. Its
[Policy Coverage rule](https://support.google.com/googleplay/android-developer/answer/10146128?hl=en)
warns that apps providing access to third-party games without installation may
receive additional review. This makes game-branded marketing the largest Play
approval risk.

## Google Play policy and commercial model

### Emulator/ROM execution

The current policy set does not expressly prohibit a CPU/GPU emulator. ROMs
run inside WhittyArcade's interpreters and do not become Android DEX/JAR/SO
code or gain access to Android APIs. That should fit the virtual-machine/
interpreter exception in the
[Device and Network Abuse policy](https://support.google.com/googleplay/android-developer/answer/16559646?hl=en),
but this is an inference, not advance approval.

To preserve that position:

- never download ROMs, firmware, native libraries or app updates in the app;
- accept only explicit user selections through the Storage Access Framework;
- treat imported data as untrusted and keep it inside the emulated machine;
- do not expose an emulator-to-Android scripting/API bridge;
- update the application and native code only through Play;
- explain the local interpreter model in reviewer notes.

### Recommended sale model

Use a one-time paid app with all emulator functionality enabled:

- Google Play handles the paid download; no in-app BillingClient integration
  is needed when there are no later digital unlocks;
- any future feature/game-pack unlock sold in the Play build must use an
  allowed Play billing route and cannot contain third-party game data;
- a service fee applies and a verified merchant payments profile/bank account
  is required;
- do not first publish the production package as free: Play permits paid to
  free, but a package once offered free cannot later become paid;
- keep the Play Android package off the public GitHub binary release if the
  business decision is to charge for it. Linux/Windows may use different
  pricing without violating Play's current payments rule;
- if an Android demo is necessary, use internal/closed testing or a carefully
  differentiated package/product strategy reviewed for Play's repetitive
  content rules.

The governing sources are Google's current
[Payments policy](https://support.google.com/googleplay/android-developer/answer/9858738?hl=en)
and [app pricing rules](https://support.google.com/googleplay/android-developer/answer/6334373?hl=en).
Model service fees, tax and refunds using the actual Play merchant agreement
at launch; 2026 fee programmes are changing and should not be hard-coded into
a business forecast.

### Play distribution agreement and EULA

The current WhittyArcade licence is suitable as a private-source/desktop
starting point but must not be used unchanged for Play. Google's current
[Developer Distribution Agreement](https://play.google/developer-distribution-agreement.html)
includes rights and obligations that override a conflicting product EULA,
including a worldwide/perpetual end-user use licence, reinstall rights, the
selected family-sharing position, Google's hosting/review/marketing rights,
refund provisions and support obligations.

Commission a Play-specific EULA/addendum that:

- preserves personal, non-commercial end-user use and the proprietary source
  restrictions only to the extent allowed by mandatory law;
- recognises Play acquisition, reinstall and any selected family-sharing
  rights instead of the current one-backup/revocable wording;
- grants Google the distribution/operational rights accepted in the DDA;
- does not purport to restrict rights granted by third-party licences;
- clearly excludes any licence to ROMs, firmware, game assets or third-party
  marks;
- preserves statutory consumer rights, refunds and remedies;
- states support scope, compatibility limits and update policy;
- uses the correct legal seller name/address and is consistent with every
  chosen distribution country.

Google's current DDA also requires timely paid-product support. Establish a
monitored support address/process capable of ordinary responses within three
business days and urgent responses within 24 hours.

### Privacy and permissions

The present native application is unusually favourable for privacy:

- ROM ZIPs, settings, NVRAM and scores are processed locally;
- there is no app account, advertising ID, telemetry or crash-reporting SDK;
- no application networking code was found;
- the public website's aggregate GitHub download counter is not embedded in
  the application.

Preserve that position in Android:

- use `ACTION_OPEN_DOCUMENT`/`ACTION_OPEN_DOCUMENT_TREE` and app-private
  storage;
- do not request `MANAGE_EXTERNAL_STORAGE`, contacts, location, microphone,
  camera, installed-package visibility or advertising permissions;
- request no Internet permission unless a reviewed feature genuinely needs
  it;
- keep ROM paths, hashes, play history and controller data on-device;
- perform a permission and network diff on the final AAB, including every SDK.

On-device-only processing is not declared as collection in Google's Data
Safety definitions, but every app still needs the form and a public privacy
policy URL even when it collects nothing:
[Data Safety requirements](https://support.google.com/googleplay/android-developer/answer/10787469?hl=en).
Publish a matching in-app Privacy page stating what local files are accessed,
that they are not uploaded, retention/deletion behaviour, support contact and
the effect of Android backups if enabled.

Play Integrity is optional. It can return an app-licensing verdict and prompt
an unlicensed user to acquire the Play copy:
[licensing verdict](https://developer.android.com/google/play/integrity/verdicts?hl=en).
It also introduces Google Play service processing, privacy declarations,
network/offline failure modes and usually a backend if enforcement is to be
meaningful. Decide after the first paid beta. If adopted, use the licensing
signal proportionately, cache a reasonable offline grace period, and do not
exclude legitimate users merely for a rooted/custom device unless fraud data
justifies it.

### Target audience and rating

- Declare the product as not designed for children unless the actual product
  strategy and content justify the Families obligations.
- Complete IARC accurately. Because the supported experience includes a
  lightgun game and can display user-supplied game violence, do not assume
  "no bundled ROM" automatically earns an Everyone rating.
- Declare no ads while the app contains no advertising SDK or placements.
- Keep the highest content the app can intentionally display in mind when
  selecting countries and target ages.

Google requires the target-audience declaration and content-rating
questionnaire:
[content ratings](https://support.google.com/googleplay/android-developer/answer/9859655?hl=en).

## Store-review functionality without copyrighted ROMs

A fresh install currently has useful ROM-management UI but cannot demonstrate
actual emulation without data that Google reviewers should not be given by us.
That creates quality/review friction even though it is correct legally.

Add an original `WhittyArcade Diagnostics` mode which needs no external ROM
and is clearly not an arcade-game dump. It should:

- render an original animated test scene through the same Android GPU path;
- exercise audio with an original/generated test tone;
- show the P1/P2 crosshairs and touch/controller input state;
- demonstrate the 800 x 720 launcher and 560 x 510 modal layout;
- exercise pause/resume, safe-area and lifecycle handling;
- explain ROM import and the legal-data requirement without naming download
  sources;
- make the review build meaningfully functional without hidden credentials or
  copyrighted test files.

This is a review-risk mitigation based on Google's requirement for stable,
responsive and meaningful functionality, not permission to bundle a game:
[Functionality policy](https://support.google.com/googleplay/android-developer/answer/9898783?hl=en).
Provide concise Play Console reviewer notes describing diagnostics, the local
document picker and why no commercial ROM is supplied.

## Android/Play technical release gate

The Android-first build scope in
[`cross_platform_build_scope.md`](cross_platform_build_scope.md) remains the
engineering base, with these Play-specific additions:

### Package and platform

- Reserve and register a durable package such as
  `com.crownparkcomputing.whittyarcade`; never put a third-party game/brand in
  the package name.
- Use `minSdk 26`, `arm64-v8a` first and a declared GLES 3.2 requirement.
- Target Android 16/API 36 from the start because submissions on or after
  31 August 2026 require it:
  [target API schedule](https://support.google.com/googleplay/android-developer/answer/11926878?hl=en-GB_ALL).
- Build every native library for 16 KB pages; this has been required for new
  Play submissions targeting Android 15+ since November 2025:
  [16 KB guidance](https://developer.android.com/guide/practices/page-sizes).
- Produce an Android App Bundle and enrol in Play App Signing. New Play apps
  use AAB delivery:
  [bundle requirement](https://support.google.com/googleplay/android-developer/answer/9844679?hl=en)
  and [Play App Signing](https://support.google.com/googleplay/android-developer/answer/9842756?hl=en).
- Register the package/identity before the 30 September 2026 Android developer
  verification deadline shown in the current
  [Play package guidance](https://support.google.com/googleplay/android-developer/answer/16984799?hl=en).

### Release hardening

- use release optimisation/LTO where stable, hidden symbol visibility, stack
  protection, RELRO and non-executable memory defaults;
- strip public native libraries but archive/upload matching native debug
  symbols privately for Play crash symbolication;
- enable R8 for the Java/Kotlin shell without obscuring required SDL/JNI entry
  points;
- scan the final AAB/APKs for unexpected files, permissions, URLs, SDKs,
  native libraries, licences and debug strings;
- fuzz and ASan/UBSan-test ZIP central-directory parsing, archive entry reads,
  ROM manifests and malformed/truncated inputs before ARM64 release;
- test no-ROM startup, diagnostics, SAF import, every board smoke path,
  Time Crisis touch/pedal/crosshair, controller hot-plug, audio focus,
  suspend/resume, low storage and corrupt ZIP handling;
- use Play pre-launch reports and Android vitals; diagnose native crashes with
  uploaded symbols, never by shipping an unstripped engine.

## Play Console/business prerequisites

For a CrownParkComputing commercial product, prefer an organisation account if
there is a real registered organisation/business able to pass verification.
Google requires organisation identity documents and a D-U-N-S number; merchant
developers also need a verified payments profile and bank/payment method:
[identity requirements](https://support.google.com/googleplay/android-developer/answer/10841920?hl=en)
and [payments profile](https://support.google.com/googleplay/android-developer/answer/7161426?hl=en).

Before creating the package:

- decide the legal seller and Play account type with an accountant/solicitor;
- make the business registry, D-U-N-S, bank, tax and payments-profile names and
  addresses match exactly;
- accept that verified developer/support contact information is public;
- use role-separated Play Console accounts, hardware-backed 2FA and least
  privilege;
- protect the upload/app-signing keys and document recovery;
- establish sales bookkeeping, applicable VAT/tax treatment, refund handling,
  privacy/support inboxes and incident response;
- choose distribution countries only after checking consumer/IP obligations
  there.

If the account is a new personal account created after 13 November 2023,
production access requires a closed test with at least 12 continuously opted-in
testers for 14 days, followed by an application for production access:
[testing requirement](https://support.google.com/googleplay/android-developer/answer/14151465?hl=en).
Run a meaningful closed test even if an organisation account is not subject to
that mandatory minimum.

## Required go/no-go evidence

### Legal and ownership

- [ ] Seller/legal entity and copyright ownership documented.
- [ ] Contributor/contractor assignments complete.
- [ ] WhittyArcade/CrownParkComputing trade-mark clearance complete.
- [ ] Per-file MAME/adaptation provenance and licence opinion complete.
- [ ] SoftFloat decision approved.
- [ ] Android dependency/SBOM and notices approved.
- [ ] Game/board-name nominative-use and store-copy opinion complete for every
      distribution territory selected.
- [ ] No third-party game screenshots/audio/logos in submitted or promotional
      assets unless written permission is on file.
- [ ] Play EULA/addendum, privacy policy, support/refund policy and consumer
      terms approved.

### Product and security

- [ ] Android foundation and GLES device spike pass.
- [ ] AAB contains no ROM, firmware, keys or accidental private source.
- [ ] Original diagnostic mode gives reviewers meaningful no-ROM function.
- [ ] SAF is the only broad user-file entry point; no all-files permission.
- [ ] Final permission/network/SDK audit matches Data Safety declarations.
- [ ] Malformed-archive fuzz/sanitizer gate passes.
- [ ] Physical Adreno and Mali acceptance matrix passes with 16 KB validation.
- [ ] Licences/privacy/support are reachable inside the app.
- [ ] Release binary is stripped and private symbols are retained/uploaded.

### Store and operations

- [ ] Organisation/personal account selection and identity verification pass.
- [ ] Package registered; app signing and upload-key recovery documented.
- [ ] Payments profile, bank and tax configuration verified.
- [ ] Listing explicitly says no games/firmware and uses only original assets.
- [ ] Ads, Data Safety, target audience, IARC, app access/reviewer notes and
      permissions declarations are complete and mutually consistent.
- [ ] Closed test/production-access requirement is satisfied where applicable.
- [ ] Support response, refunds, crash triage and target-API maintenance have
      named owners.
- [ ] IP evidence pack is ready for advance Play contact or an appeal.

Production is a **go** only when every checkbox above has evidence. A signed
Android build alone is not a commercial-release gate.

## Estimate

The existing Android scope estimates 28-48 developer days for shared
foundation, renderer proof and a usable ARM64 build. Play sale readiness adds:

| Additional work | Estimate |
|---|---:|
| Build-closure SBOM, per-file provenance and notice generator | 4-7 developer days |
| SoftFloat removal/validation and permissive audio dependency profile | 5-12 developer days |
| Original diagnostics/reviewer mode | 3-5 developer days |
| Play EULA/privacy/licences/about/support surfaces and Console automation | 3-6 developer days plus external legal review |
| Archive fuzzing, AAB audit, Play device/closed-test fixes | 5-10 developer days |

The straight sum is 48-88 developer days. Some dependency, diagnostics and QA
work can overlap the Android foundation, but commercial planning should retain
contingency: budget roughly **50-90 developer days** from Preview 2 to a
credible paid Play candidate. This excludes unpredictable time to obtain
publisher permissions, external legal review, account/D-U-N-S verification,
Google review and the mandatory 14-day closed-test period when it applies.

## Recommended sequence

1. Commission the short IP/licence/EULA opinion before Android renderer work
   becomes a large sunk cost.
2. Establish the legal seller, trade-mark clearance and Play organisation
   account/package reservation.
3. Build the third-party provenance manifest and prove the commercial link
   closure; remove SoftFloat and avoid LGPL where practical.
4. Implement the shared Android foundation/GLES renderer and local-only SAF
   storage from the cross-platform scope.
5. Add original diagnostics, privacy/licence surfaces and release hardening.
6. Produce the AAB, symbols, SBOM and rights evidence pack; run internal and
   physical-device testing.
7. Submit an independently branded closed-test listing with no commercial
   game assets and complete every policy declaration.
8. Treat any IP review question as a stop condition: supply evidence or narrow
   the listing/product; do not repeatedly resubmit unsupported game branding.
9. Move to a staged paid production rollout only after legal, crash, refund
   and support gates are green.
