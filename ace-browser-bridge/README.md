# ACE Browser Bridge

Minimal Chrome MV3 extension prototype for ACECode browser control experiments.

When the extension popup opens, it injects `content/virtual-cursor.js` into the
current tab and starts a virtual cursor that moves randomly around the page.

## Load in Chrome

1. Open `chrome://extensions`.
2. Enable `Developer mode`.
3. Click `Load unpacked`.
4. Select this directory: `C:\Users\shao\acecode\ace-browser-bridge`.
5. Open a normal web page and click the `ACE Browser Bridge` extension icon.

Chrome blocks extension script injection on internal pages such as
`chrome://extensions`, `chrome://newtab`, and the Chrome Web Store. Use a normal
`http://` or `https://` page for testing.

## Files

- `manifest.json`: MV3 extension declaration.
- `popup.html`, `popup.css`, `popup.js`: extension popup that starts/stops the cursor.
- `content/virtual-cursor.js`: injected page overlay and random movement loop.

## Next Steps

- Add a service worker for long-running browser bridge state.
- Add native messaging so ACECode can send browser tool requests.
- Replace random movement with real tool actions such as click, type, and scroll.
