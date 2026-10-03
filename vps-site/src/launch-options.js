const STORAGE_KEY = 'botty.launch-services.v1';
const SERVICES = ['ftp', 'rtorrent', 'cheatrunner'];

export function normalizeLaunchServices(value) {
  return Object.fromEntries(SERVICES.map(name => [name, value?.[name] !== false]));
}

export function bindLaunchOptions(document, browser) {
  const fieldset = document.getElementById('launch-services');
  const inputs = SERVICES.map(name => fieldset.querySelector('input[name="' + name + '"]'));
  const storageStatus = document.getElementById('launch-options-storage');
  let services;
  try {
    services = normalizeLaunchServices(JSON.parse(browser.localStorage.getItem(STORAGE_KEY)));
  } catch {
    services = normalizeLaunchServices();
    storageStatus.hidden = false;
    storageStatus.textContent = 'Choices apply to this launch. Browser storage is unavailable or saved choices could not be read.';
  }
  const read = () => Object.fromEntries(inputs.map(input => [input.name, input.checked]));
  const summarize = () => {
    const count = inputs.filter(input => input.checked).length;
    document.getElementById('launch-options-summary').textContent = count + ' of 3 services enabled';
  };
  for (const input of inputs) {
    input.checked = services[input.name];
    input.addEventListener('change', () => {
      summarize();
      try {
        browser.localStorage.setItem(STORAGE_KEY, JSON.stringify(read()));
        storageStatus.hidden = true;
        storageStatus.textContent = '';
      } catch {
        storageStatus.hidden = false;
        storageStatus.textContent = 'Choices apply to this launch only. Browser storage is unavailable.';
      }
    });
  }
  summarize();
  return { lock() {
    const selected = read();
    fieldset.disabled = true;
    document.getElementById('launch-options').open = false;
    return selected;
  } };
}
