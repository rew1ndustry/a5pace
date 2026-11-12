fetch('/api/line').then(r=>r.text()).then(t=>{document.getElementById('o').textContent=t})
