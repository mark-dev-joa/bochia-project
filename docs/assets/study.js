const toc = document.querySelector('#toc');
document.querySelectorAll('article h2, article h3').forEach((heading, index) => {
  heading.id ||= `section-${index + 1}`;
  const link = document.createElement('a');
  link.href = `#${heading.id}`;
  link.textContent = heading.textContent;
  if (heading.tagName === 'H3') link.className = 'subheading';
  toc.append(link);
});
document.querySelectorAll('article table').forEach(table => {
  const wrap = document.createElement('div');
  wrap.className = 'table-wrap';
  table.before(wrap);
  wrap.append(table);
});
const blocks = [...document.querySelectorAll('pre code.language-mermaid')];
if (blocks.length) {
  try {
    const { default: mermaid } = await import('https://cdn.jsdelivr.net/npm/mermaid@11.12.0/dist/mermaid.esm.min.mjs');
    mermaid.initialize({startOnLoad: false, securityLevel: 'strict', theme: 'base', themeVariables: {
      primaryColor: '#eef4ef', primaryTextColor: '#213c37', primaryBorderColor: '#a9c0b3',
      lineColor: '#718c7f', fontFamily: 'Segoe UI, Malgun Gothic, sans-serif', fontSize: '15px'
    }, flowchart: {htmlLabels: false, curve: 'linear', padding: 14, rankSpacing: 24}});
    const nodes = blocks.map(code => {
      const box = document.createElement('div');
      box.className = 'mermaid';
      box.textContent = code.textContent;
      code.parentElement.replaceWith(box);
      return box;
    });
    await mermaid.run({nodes});
  } catch (error) {
    const note = document.createElement('p');
    note.className = 'diagram-status';
    note.textContent = '흐름도를 불러오지 못했습니다. 아래 번호별 표에서 같은 절차를 확인할 수 있습니다.';
    document.querySelector('article').prepend(note);
    console.error('Diagram rendering failed', error);
  }
}
