import { test, expect } from '@playwright/test';
import AxeBuilder from '@axe-core/playwright';

test('the production site loads all local assets under the Pages subpath', async ({ page }, testInfo) => {
  const errors = [];
  const failures = [];
  page.on('pageerror', (error) => errors.push(error.message));
  page.on('console', (message) => {
    if (message.type() === 'error') errors.push(message.text());
  });
  page.on('requestfailed', (request) => failures.push(request.url()));
  page.on('response', (response) => {
    if (response.status() >= 400) failures.push(`${response.status()} ${response.url()}`);
  });
  await page.goto('./');
  await expect(page.getByRole('heading', { level: 1 })).toHaveText('A new DAWn for music production');
  for (const image of await page.locator('img[src]').all()) {
    await image.scrollIntoViewIfNeeded();
    await image.evaluate((element) => element.decode());
  }
  await page.evaluate(() => document.fonts.ready);
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
  expect(errors).toEqual([]);
  expect(failures).toEqual([]);
  await page.screenshot({ path: testInfo.outputPath('website.png'), fullPage: true });
});

test('section navigation and download links point to working destinations', async ({ page }) => {
  await page.goto('./');
  const missingAnchors = await page.locator('a[href^="#"]').evaluateAll((links) =>
    links.filter((link) => !document.getElementById(link.hash.slice(1))).map((link) => link.hash));
  expect(missingAnchors).toEqual([]);
  await page.getByRole('navigation', { name: 'Main navigation' }).getByRole('link', { name: 'Our mission' }).click();
  await expect(page).toHaveURL(/#mission$/);
  await expect(page.getByRole('heading', { name: 'Record, edit, mix, and master.' })).toBeInViewport();
  await page.getByRole('link', { name: 'Download for macOS or Windows' }).scrollIntoViewIfNeeded();
  await expect(page.getByRole('link', { name: 'Download for macOS or Windows' })).toHaveAttribute(
    'href', 'https://github.com/mbianchidev/studio-duo/releases/latest');
});

test('every screenshot opens, closes, and restores keyboard focus', async ({ page }) => {
  await page.goto('./');
  const links = page.locator('.screenshot-link');
  for (let index = 0; index < await links.count(); index++) {
    const link = links.nth(index);
    await link.focus();
    await link.press('Enter');
    const dialog = page.getByRole('dialog');
    await expect(dialog).toBeVisible();
    await expect(dialog.locator('img')).toHaveAttribute('alt', await link.locator('img').getAttribute('alt'));
    await dialog.locator('img').evaluate((image) => image.decode());
    await expect(dialog.getByRole('button', { name: 'Close screenshot' })).toBeFocused();
    await page.keyboard.press('Escape');
    await expect(dialog).not.toBeVisible();
    await expect(link).toBeFocused();
    await expect(page.locator('body')).not.toHaveClass(/screenshot-open/);
  }
  await links.first().click();
  await page.getByRole('button', { name: 'Close screenshot' }).click();
  await expect(page.getByRole('dialog')).not.toBeVisible();
  await links.first().click();
  await page.mouse.click(1, 1);
  await expect(page.getByRole('dialog')).not.toBeVisible();
});

test('content and the screenshot fallback work without JavaScript', async ({ browser, baseURL, viewport, isMobile, hasTouch }) => {
  const context = await browser.newContext({ javaScriptEnabled: false, viewport, isMobile, hasTouch });
  try {
    const page = await context.newPage();
    await page.goto(baseURL);
    await expect(page.getByRole('heading', { level: 1 })).toBeVisible();
    await expect(page.getByRole('heading', { name: 'Download Studio Duo.' })).toBeAttached();
    await page.getByRole('link', { name: 'Enlarge the Studio Duo session screenshot' }).click();
    await expect(page).toHaveURL(/\/studio-duo\/images\/session\.webp$/);
  } finally {
    await context.close();
  }
});

test('the page and screenshot viewer meet automated WCAG AA checks', async ({ page }) => {
  await page.goto('./');
  const options = ['wcag2a', 'wcag2aa', 'wcag21aa', 'wcag22aa'];
  const pageResults = await new AxeBuilder({ page }).withTags(options).analyze();
  expect(pageResults.violations).toEqual([]);
  await page.getByRole('link', { name: 'Enlarge the drum geometry screenshot' }).click();
  const dialogResults = await new AxeBuilder({ page }).withTags(options).analyze();
  expect(dialogResults.violations).toEqual([]);
});

test('keyboard navigation exposes the skip link and respects reduced motion', async ({ page }) => {
  await page.emulateMedia({ reducedMotion: 'reduce' });
  await page.goto('./');
  await page.keyboard.press('Tab');
  await expect(page.getByRole('link', { name: 'Skip to content' })).toBeFocused();
  await page.keyboard.press('Enter');
  await expect(page).toHaveURL(/#main$/);
  expect(await page.evaluate(() => getComputedStyle(document.documentElement).scrollBehavior)).toBe('auto');
});

test('screenshots can be enlarged and closed by touch', async ({ page, hasTouch }) => {
  test.skip(!hasTouch, 'Touch interaction applies to tablet and phone layouts.');
  await page.goto('./');
  await page.getByRole('link', { name: 'Enlarge the drum geometry screenshot' }).tap();
  await expect(page.getByRole('dialog')).toBeVisible();
  await page.getByRole('button', { name: 'Close screenshot' }).tap();
  await expect(page.getByRole('dialog')).not.toBeVisible();
});

test('platform expectations, built-in features, and partner tools are explicit', async ({ page }) => {
  await page.goto('./');
  await expect(page.locator('.hero-copy')).toContainText('Open source core. No required account. No AI features built in.');
  await expect(page.locator('.platform-note')).toContainText('Windows support is best effort only');
  await expect(page.locator('.platform-note')).toContainText('no Windows testing device');
  await expect(page.locator('#record')).toContainText('ASIO-compatible interfaces on Windows');
  await expect(page.locator('#record')).toContainText('Focusrite Scarlett Duo on macOS ARM');
  await expect(page.locator('#record')).toContainText('using CoreAudio');
  await expect(page.locator('#drums')).toContainText('record native MIDI drums');
  await expect(page.locator('#drums')).toContainText('kit of twelve performance pads visible');
  await expect(page.locator('#drums')).toContainText('metronome');
  await expect(page.locator('#mix')).toContainText('parametric EQ');
  await expect(page.locator('#mix')).toContainText('signal generator');
  await expect(page.locator('#master')).toContainText('WAV, AIFF, FLAC, Ogg Vorbis, or MP3');
  await expect(page.locator('#system-requirements')).toContainText('macOS 12 Monterey or later');
  await expect(page.locator('#system-requirements')).toContainText('Apple silicon ARM64 and Intel x86_64');
  await expect(page.locator('#system-requirements')).toContainText('Windows 10 or 11');
  await expect(page.locator('#system-requirements')).toContainText('Visual C++ x64 Runtime');
  await expect(page.locator('#system-requirements')).toContainText('Linux is unsupported');
  await expect(page.locator('a[href*="github.com/mbianchidev/scream-forge"]')).toHaveCount(0);
  await expect(page.locator('#scream-forge-contact')).toHaveAttribute('href', /^mailto:[^@\s]+@[^@\s]+$/);
  await expect(page.getByRole('link', { name: 'Explore Track2Stem' })).toHaveAttribute(
    'href', 'https://github.com/mbianchidev/track2stem');
  await expect(page.locator('.partner-tools')).toContainText('not inside Studio Duo');
});

test('the headline preserves DAWn case, the tagline sits below it, and copyright remains visible', async ({ page }) => {
  await page.goto('./');
  const headline = page.getByRole('heading', { level: 1 });
  await expect(headline).toHaveText('A new DAWn for music production');
  await expect(page.locator('.hero-tagline')).toHaveText('Your music. Your decisions.');
  expect(await headline.evaluate((element) => getComputedStyle(element).textTransform)).toBe('none');
  await page.evaluate(() => document.fonts.ready);
  const headlineBox = await headline.boundingBox();
  const taglineBox = await page.locator('.hero-tagline').boundingBox();
  expect(taglineBox.y).toBeGreaterThanOrEqual(headlineBox.y + headlineBox.height);
  await page.locator('.copyright').scrollIntoViewIfNeeded();
  await expect(page.locator('.copyright')).toBeVisible();
  await expect(page.locator('.copyright')).toHaveText('\u00a9 2026 SyncTune');
});

test('the feature matrix compares the requested DAWs, licenses, and capabilities side by side', async ({ page }) => {
  await page.goto('./');
  const comparison = page.locator('#comparison');
  await expect(comparison).toContainText('It does less than the established full DAWs here.');
  const table = comparison.getByRole('table');
  await expect(table.getByRole('columnheader')).toHaveText([
    'Feature',
    'Studio Duo',
    'Fender Studio Pro Formerly Studio One',
    'Ableton Live 12 Suite',
    'Pro Tools Studio',
    'REAPER 7',
    'FL Studio Producer Edition',
  ]);
  await expect(table.getByRole('rowheader')).toHaveText([
    'License model',
    'Desktop platforms',
    'Multitrack audio recording',
    'MIDI recording and piano roll',
    'Take comping',
    'Drum programming and instruments',
    'Built-in EQ and effects',
    'Third-party plug-in formats',
    'Mixing and automation',
    'Mastering workflow',
  ]);
  for (const row of await table.locator('tbody tr').all()) {
    await expect(row.getByRole('rowheader')).toHaveCount(1);
    await expect(row.getByRole('cell')).toHaveCount(6);
    for (const cell of await row.getByRole('cell').all()) {
      await expect(cell).not.toBeEmpty();
    }
  }
  const license = table.getByRole('row').filter({ has: page.getByRole('rowheader', { name: 'License model', exact: true }) });
  await expect(license.getByRole('cell').nth(0)).toContainText('AGPLv3 open source core');
  await expect(license.getByRole('cell').nth(5)).toContainText('Lifetime Free Updates');
  const platforms = table.getByRole('row').filter({ has: page.getByRole('rowheader', { name: 'Desktop platforms', exact: true }) });
  await expect(platforms.getByRole('cell').nth(0)).toContainText('Windows best effort');
  await expect(platforms.getByRole('cell').nth(0)).toContainText('Linux unsupported');
  await expect(platforms.getByRole('cell').nth(1)).toContainText('Linux public beta');
  await expect(platforms.getByRole('cell').nth(4)).toContainText('Linux');
  const plugins = table.getByRole('row').filter({ has: page.getByRole('rowheader', { name: 'Third-party plug-in formats', exact: true }) });
  await expect(plugins.getByRole('cell').nth(0)).toContainText('No AAX hosting');
  await expect(plugins.getByRole('cell').nth(5)).toContainText('CLAP');
  await expect(table.locator('caption')).toContainText('Sources checked 12 October 2026');
  await expect(comparison).not.toContainText('Free recording app');
  const sources = comparison.locator('details');
  await sources.locator('summary').focus();
  await page.keyboard.press('Enter');
  await expect(sources).toHaveAttribute('open', '');
  await expect(sources.getByRole('link', { name: 'edition boundaries' })).toBeVisible();
  const violations = await new AxeBuilder({ page }).withTags(['wcag2a', 'wcag2aa', 'wcag21aa', 'wcag22aa']).analyze();
  expect(violations.violations).toEqual([]);
});

test('the wide feature matrix keeps row labels visible and scrolls by keyboard', async ({ page }) => {
  await page.emulateMedia({ reducedMotion: 'reduce' });
  await page.goto('./');
  const comparison = page.locator('#comparison');
  const table = comparison.getByRole('table');
  const scrollRegion = comparison.getByRole('region', { name: 'DAW comparison, scroll horizontally' });
  await scrollRegion.focus();
  await expect(scrollRegion).toBeFocused();
  const rowLabel = table.getByRole('rowheader', { name: 'License model', exact: true });
  const originalLeft = (await rowLabel.boundingBox()).x;
  if (await scrollRegion.evaluate((element) => element.scrollWidth > element.clientWidth)) {
    await scrollRegion.press('ArrowRight');
    await expect.poll(() => scrollRegion.evaluate((element) => element.scrollLeft)).toBeGreaterThan(0);
    await scrollRegion.evaluate((element) => { element.scrollLeft = element.scrollWidth; });
    expect(Math.abs((await rowLabel.boundingBox()).x - originalLeft)).toBeLessThan(2);
  }
  await page.keyboard.press('Tab');
  await expect(table.getByRole('link', { name: 'Studio Duo', exact: true })).toBeFocused();
  const lastSource = table.getByRole('link', { name: 'FL Studio', exact: true });
  await lastSource.focus();
  await expect(lastSource).toBeInViewport();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
});
