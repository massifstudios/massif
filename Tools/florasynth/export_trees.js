#!/usr/bin/env node
//
// Batch-exports tree models from the Florasynth browser editor (florasynth.com).
//
// The editor is a Vue + three.js app that meshes trees client-side and exports
// them with its own OBJ/GLTF writers. This script drives that UI headlessly and
// captures the resulting file instead of letting it land in ~/Downloads.
//
// Exporting requires an active Florasynth browser subscription. Run the `login`
// command once to sign in manually; the session is reused from auth.json.

const fs = require('fs');
const path = require('path');
const readline = require('readline');
const { chromium } = require('playwright');

const EDITOR_URL = 'https://www.florasynth.com/editor?new=true';
const LOGIN_URL = 'https://www.florasynth.com/login';
const AUTH_FILE = path.join(__dirname, 'auth.json');
const DEFAULT_OUTPUT_DIR = path.join(__dirname, 'out');

const SPECIES = [
  'Green Ash', 'White Oak', 'Aspen Grove', 'Colorado Spruce', 'Dogwood',
  'Douglas Fir', 'Honey Locust', 'Lombardy Poplar', 'Peachleaf Willow',
  'Pin Oak', 'Quaking Aspen', 'Red Maple', 'Weeping Willow',
];

const GENERATION_TIMEOUT_MS = 300000;

// The app revokes its blob URL on the same tick it clicks the download anchor,
// which can race Chromium's download handler. Defer revocation until we have
// the file, then release the blobs so long runs do not accumulate them.
const DEFER_BLOB_REVOCATION = () => {
  const originalRevoke = URL.revokeObjectURL.bind(URL);
  const pendingUrls = [];
  URL.revokeObjectURL = (url) => pendingUrls.push(url);
  window.__florasynthFlushRevoke = () => pendingUrls.splice(0).forEach(originalRevoke);
};

function resolveChromiumExecutable() {
  const override = process.env.FLORASYNTH_CHROME;
  if (override) {
    return override;
  }
  const bundled = chromium.executablePath();
  if (fs.existsSync(bundled)) {
    return bundled;
  }
  return findCachedChromium();
}

function findCachedChromium() {
  const cacheRoot = path.join(process.env.HOME, '.cache', 'ms-playwright');
  if (!fs.existsSync(cacheRoot)) {
    throw new Error('No Chromium found. Run: npx playwright install chromium');
  }
  const candidates = fs.readdirSync(cacheRoot)
    .filter((entry) => /^chromium-\d+$/.test(entry))
    .map((entry) => path.join(cacheRoot, entry, 'chrome-linux64', 'chrome'))
    .filter((candidate) => fs.existsSync(candidate));
  if (candidates.length === 0) {
    throw new Error('No Chromium found. Run: npx playwright install chromium');
  }
  return candidates[candidates.length - 1];
}

function launchBrowser(headless) {
  return chromium.launch({
    headless,
    executablePath: resolveChromiumExecutable(),
    // The editor needs a real WebGL context even when headless.
    args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'],
  });
}

function parseArguments(argv) {
  const options = {};
  for (let index = 0; index < argv.length; index += 1) {
    if (!argv[index].startsWith('--')) {
      continue;
    }
    const key = argv[index].slice(2);
    const next = argv[index + 1];
    const isFlag = next === undefined || next.startsWith('--');
    options[key] = isFlag ? true : next;
  }
  return options;
}

function promptForEnter(message) {
  const input = readline.createInterface({ input: process.stdin, output: process.stdout });
  return new Promise((resolve) => input.question(message, () => {
    input.close();
    resolve();
  }));
}

async function login() {
  const browser = await launchBrowser(false);
  const context = await browser.newContext({ viewport: { width: 1280, height: 900 } });
  const page = await context.newPage();
  await page.goto(LOGIN_URL, { waitUntil: 'domcontentloaded' });

  console.log('\nA browser window is open. Sign in to Florasynth yourself.');
  console.log('This script never sees or stores your password — only the session cookies.');
  await promptForEnter('\nPress Enter here once you are signed in... ');

  await context.storageState({ path: AUTH_FILE });
  await browser.close();
  console.log(`Session saved to ${AUTH_FILE}`);
}

async function openEditor(context) {
  const page = await context.newPage();
  await page.goto(EDITOR_URL, { waitUntil: 'domcontentloaded', timeout: 60000 });
  await page.locator('.additional_stats').filter({ hasText: 'vertices' })
    .waitFor({ timeout: GENERATION_TIMEOUT_MS });
  await dismissReleaseNotes(page);
  return page;
}

async function dismissReleaseNotes(page) {
  const okButton = page.getByRole('button', { name: 'Ok', exact: true });
  const isVisible = await okButton.isVisible().catch(() => false);
  if (!isVisible) {
    return;
  }
  await okButton.click();
  await page.waitForTimeout(300);
}

async function waitForGeneration(page) {
  const loadingBar = page.locator('.growth_loading_bar');
  await loadingBar.waitFor({ state: 'visible', timeout: 15000 }).catch(() => {});
  await loadingBar.waitFor({ state: 'hidden', timeout: GENERATION_TIMEOUT_MS });
  // The stats readout and the meshes settle a frame or two after the bar hides.
  await page.waitForTimeout(1500);
}

async function selectSpecies(page, speciesName) {
  await page.locator('.species_card').first().click({ force: true });
  await page.locator('.items_list').first().waitFor({ timeout: 15000 });
  await page.locator('.items_list').first().getByText(speciesName, { exact: true }).first().click();
  await waitForGeneration(page);
}

async function jumpToAge(page, ageInYears) {
  await page.locator('.jump-input').fill(String(ageInYears));
  await page.locator('.jump-button').click();
  await waitForGeneration(page);
}

async function newVariation(page) {
  await page.getByRole('button', { name: 'New Variation', exact: true }).click();
  await waitForGeneration(page);
}

async function openExportPanel(page) {
  await page.locator('.editor_tools_nav h3', { hasText: 'Exporting' }).first().click();
  await page.waitForTimeout(500);
  await assertExportUnlocked(page);
}

async function assertExportUnlocked(page) {
  const paywallNotice = page.locator('.export_notice');
  if (await paywallNotice.isVisible().catch(() => false)) {
    throw new Error('Florasynth reports no active subscription for this session. '
      + 'Export is a premium feature — check the account, then re-run the login command.');
  }
  const loginPrompt = page.getByRole('link', { name: /Get Premium to Export|Login to save/ });
  if (await loginPrompt.isVisible().catch(() => false)) {
    throw new Error('Not signed in. Run: node export_trees.js login');
  }
}

async function captureDownload(page, clickTarget, outputPath) {
  const [download] = await Promise.all([
    page.waitForEvent('download', { timeout: GENERATION_TIMEOUT_MS }),
    clickTarget.click(),
  ]);
  await download.saveAs(outputPath);
  await page.evaluate(() => window.__florasynthFlushRevoke());
  return fs.statSync(outputPath).size;
}

async function exportModel(page, format, outputPath) {
  const buttonName = format === 'obj' ? 'OBJ' : 'GLTF';
  const button = page.getByRole('button', { name: buttonName, exact: true });
  return captureDownload(page, button, outputPath);
}

async function exportParameters(page, outputPath) {
  const button = page.getByRole('button', { name: 'Export JSON', exact: true });
  return captureDownload(page, button, outputPath);
}

function slugify(name) {
  return name.replace(/\s+/g, '');
}

// OBJ export bundles the .obj, .mtl and texture maps into a single zip.
function fileExtensionFor(format) {
  return format === 'obj' ? 'zip' : 'gltf';
}

async function exportSpecies(page, speciesName, settings) {
  await selectSpecies(page, speciesName);
  if (settings.age) {
    await jumpToAge(page, settings.age);
  }
  await openExportPanel(page);

  for (let variation = 1; variation <= settings.variations; variation += 1) {
    await exportVariation(page, speciesName, variation, settings);
    const isLastVariation = variation === settings.variations;
    if (!isLastVariation) {
      await newVariation(page);
    }
  }
}

async function exportVariation(page, speciesName, variation, settings) {
  const baseName = `SM_Tree_${slugify(speciesName)}_${settings.age}y_${String(variation).padStart(2, '0')}`;
  const modelPath = path.join(settings.outputDir, `${baseName}.${fileExtensionFor(settings.format)}`);
  const bytes = await exportModel(page, settings.format, modelPath);
  console.log(`  ${path.basename(modelPath)}  ${(bytes / 1048576).toFixed(2)} MB`);

  if (settings.saveParameters) {
    const parametersPath = path.join(settings.outputDir, `${baseName}.params.json`);
    await exportParameters(page, parametersPath);
  }
}

function resolveSpeciesList(options) {
  if (options.all) {
    return SPECIES;
  }
  if (typeof options.species !== 'string') {
    return [SPECIES[0]];
  }
  return options.species.split(',').map((name) => name.trim()).filter(Boolean);
}

async function exportAll(options) {
  if (!fs.existsSync(AUTH_FILE)) {
    throw new Error(`No saved session at ${AUTH_FILE}. Run: node export_trees.js login`);
  }
  const settings = {
    format: options.format === 'obj' ? 'obj' : 'gltf',
    age: Number(options.age || 30),
    variations: Number(options.variations || 1),
    outputDir: path.resolve(options.out || DEFAULT_OUTPUT_DIR),
    saveParameters: Boolean(options['save-params']),
  };
  fs.mkdirSync(settings.outputDir, { recursive: true });

  const browser = await launchBrowser(true);
  const context = await browser.newContext({
    storageState: AUTH_FILE,
    acceptDownloads: true,
    viewport: { width: 1600, height: 1000 },
  });
  await context.addInitScript(DEFER_BLOB_REVOCATION);

  const page = await openEditor(context);
  const speciesList = resolveSpeciesList(options);
  console.log(`Exporting ${speciesList.length} species x ${settings.variations} `
    + `variation(s) at age ${settings.age} as ${settings.format} -> ${settings.outputDir}\n`);

  for (const speciesName of speciesList) {
    console.log(speciesName);
    await exportSpecies(page, speciesName, settings);
  }
  await browser.close();
}

function printUsage() {
  console.log(`Usage:
  node export_trees.js login
  node export_trees.js export [options]

Options:
  --all                 Export every preset species (default: Green Ash only)
  --species "A,B"       Comma-separated species names
  --age <years>         Simulated age to grow to (default: 30)
  --variations <n>      Variations per species (default: 1)
  --format gltf|obj     gltf = single file, textures embedded
                        obj  = zip containing .obj, .mtl and texture maps
  --out <dir>           Output directory (default: Tools/florasynth/out)
  --save-params         Also write the tree's parameter JSON alongside each model

Species: ${SPECIES.join(', ')}`);
}

async function main() {
  const [command, ...rest] = process.argv.slice(2);
  if (command === 'login') {
    await login();
    return;
  }
  if (command === 'export') {
    await exportAll(parseArguments(rest));
    return;
  }
  printUsage();
  process.exitCode = 1;
}

main().catch((error) => {
  console.error(`\nError: ${error.message}`);
  process.exitCode = 1;
});
