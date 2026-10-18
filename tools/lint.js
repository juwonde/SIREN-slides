// In-page check, injected by tools/build.py --lint. Appends the JSON result as a pre element (id lint-result).
(async () => {
  await document.fonts.ready;
  const MIN_PX = 20;          // 15 pt on the 1280x720 canvas
  const FOOT_TOP = 720 - 48;  // footer band
  const banned = [new RegExp("F[- ]?" + "ASTRA", "i"), new RegExp("f_" + "astra", "i"),
                  new RegExp("Direct" + "RTL", "i"), /\bbpg-r\d/i, /\brun\d{2}\b/i];
  const res = { slides: 0, small: [], outside: [], banned: [] };
  const slides = [...document.querySelectorAll(".slide")];
  res.slides = slides.length;
  slides.forEach((sl, i) => {
    const sr = sl.getBoundingClientRect();
    const walker = document.createTreeWalker(sl, NodeFilter.SHOW_TEXT);
    while (walker.nextNode()) {
      const t = walker.currentNode;
      const text = t.textContent.trim();
      if (!text) continue;
      const el = t.parentElement;
      if (el.closest("aside.notes")) continue;
      const range = document.createRange();
      range.selectNodeContents(t);
      const rects = [...range.getClientRects()].filter(r => r.width > 0 && r.height > 0);
      if (!rects.length) continue;  // hidden (other variant)
      let px = parseFloat(getComputedStyle(el).fontSize);
      if (el instanceof SVGElement) {
        const m = el.getScreenCTM();
        px *= m ? Math.hypot(m.a, m.b) : 1;
      }
      if (px < MIN_PX - 0.01) res.small.push({ slide: i + 1, px: +px.toFixed(1), text: text.slice(0, 40) });
      const inFoot = !!el.closest(".foot");
      for (const r of rects) {
        const bottomLimit = inFoot ? sr.bottom : sr.top + FOOT_TOP;
        if (r.left < sr.left - 0.5 || r.right > sr.right + 0.5 || r.top < sr.top - 0.5 || r.bottom > bottomLimit + 0.5) {
          res.outside.push({ slide: i + 1, text: text.slice(0, 40), right: Math.round(r.right - sr.left), bottom: Math.round(r.bottom - sr.top) });
          break;
        }
      }
      for (const b of banned) if (b.test(text)) res.banned.push({ slide: i + 1, text: text.slice(0, 40) });
    }
    sl.querySelectorAll(".fig svg, img").forEach(g => {
      const r = g.getBoundingClientRect();
      if (r.width && (r.right > sr.right + 0.5 || r.bottom > sr.top + FOOT_TOP + 0.5))
        res.outside.push({ slide: i + 1, text: "figure " + (g.getAttribute("data-name") || g.getAttribute("src") || ""), right: Math.round(r.right - sr.left), bottom: Math.round(r.bottom - sr.top) });
    });
  });
  const pre = document.createElement("pre");
  pre.id = "lint-result";
  pre.textContent = JSON.stringify(res);
  document.body.appendChild(pre);
})();
