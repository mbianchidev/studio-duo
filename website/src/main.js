import '@fontsource/barlow-condensed/latin-700.css';
import '@fontsource/ibm-plex-sans/latin-400.css';
import '@fontsource/ibm-plex-sans/latin-500.css';
import '@fontsource/ibm-plex-sans/latin-600.css';
import './style.css';

const dialog = document.querySelector('.screenshot-dialog');
const image = document.querySelector('.dialog-image');
const title = document.querySelector('#screenshot-title');
const caption = document.querySelector('#screenshot-caption');

if (!(dialog instanceof HTMLDialogElement)
    || !(image instanceof HTMLImageElement)
    || !(title instanceof HTMLElement)
    || !(caption instanceof HTMLElement)) {
  console.error('Screenshot viewer could not initialize. Open the screenshot links directly.');
} else {
  for (const link of document.querySelectorAll('.screenshot-link')) {
    if (!(link instanceof HTMLAnchorElement)) continue;
    link.addEventListener('click', (event) => {
      if (event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
      const thumbnail = link.querySelector('img');
      if (!(thumbnail instanceof HTMLImageElement)) {
        console.error('Screenshot link has no image. Opening the image directly.');
        return;
      }
      event.preventDefault();
      image.src = link.href;
      image.alt = thumbnail.alt;
      title.textContent = link.dataset.title;
      caption.textContent = link.dataset.caption;
      dialog.showModal();
      document.body.classList.add('screenshot-open');
    });
  }

  dialog.addEventListener('close', () => {
    document.body.classList.remove('screenshot-open');
  });

  dialog.addEventListener('click', (event) => {
    if (event.target !== dialog) return;
    const bounds = dialog.getBoundingClientRect();
    if (event.clientX < bounds.left || event.clientX > bounds.right
        || event.clientY < bounds.top || event.clientY > bounds.bottom) {
      dialog.close();
    }
  });
}
