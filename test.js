    <script>
        const symbols = ["AAPL", "GOOG", "TSLA", "MSFT", "AMZN", "NVDA", "META", "NFLX", "AMD", "INTC"];
        const companies = {
            "AAPL": "Apple Inc.", "GOOG": "Alphabet Inc.", "TSLA": "Tesla, Inc.",
            "MSFT": "Microsoft", "AMZN": "Amazon.com", "NVDA": "NVIDIA Corp",
            "META": "Meta Platforms", "NFLX": "Netflix, Inc.", "AMD": "Adv Micro Devices",
            "INTC": "Intel Corp"
        };
        
        let priceHistory = {};
        symbols.forEach(s => priceHistory[s] = []);
        
        let ws = null;
        let isConnected = false;
        let reconnectDelay = 1000;
        let selectedSymbol = null;
        
        let msgCount = 0;
        let startTime = Date.now();
        let lastMsgTime = 0;

        function initGrid() {
            const grid = document.getElementById("card-grid");
            const ticker = document.getElementById("ticker-content");
            let tickerHtml = "";
            let gridHtml = "";

            symbols.forEach(sym => {
                gridHtml += `
                    <div class="card" id="card-${sym}" onclick="selectSymbol('${sym}')">
                        <div class="card-header">
                            <div>
                                <h2 class="symbol">${sym}</h2>
                                <div class="company">${companies[sym]}</div>
                            </div>
                        </div>
                        <div class="price" id="price-${sym}">0.00</div>
                        <div class="change" id="change-${sym}">-</div>
                        <svg class="sparkline" viewBox="0 0 100 40" preserveAspectRatio="none">
                            <path id="spark-${sym}" stroke="var(--muted)"></path>
                        </svg>
                        <div class="ts" id="ts-${sym}">--</div>
                    </div>
                `;
                
                tickerHtml += `<span class="ticker-item" id="tick-${sym}">${sym} <span>0.00</span></span>`;
            });
            
            ticker.innerHTML = tickerHtml + tickerHtml;
            grid.innerHTML = gridHtml;
        }

        function updateCard(data) {
            const sym = data.symbol;
            if (!priceHistory[sym]) return;
            
            const isUp = data.change >= 0;
            const colorClass = isUp ? 'up' : 'down';
            const arrow = isUp ? '▲' : '▼';
            const glowClass = isUp ? 'glow-up' : 'glow-down';
            const strokeColor = isUp ? 'var(--green)' : 'var(--red)';

            priceHistory[sym].push(data.price);
            if (priceHistory[sym].length > 30) priceHistory[sym].shift();

            requestAnimationFrame(() => {
                const priceEl = document.getElementById(`price-${sym}`);
                const changeEl = document.getElementById(`change-${sym}`);
                const cardEl = document.getElementById(`card-${sym}`);
                const tsEl = document.getElementById(`ts-${sym}`);
                const sparkEl = document.getElementById(`spark-${sym}`);

                priceEl.innerText = data.price.toFixed(2);
                changeEl.innerHTML = `<span class="${colorClass}">${arrow} ${Math.abs(data.change).toFixed(2)}</span>`;
                
                const d = new Date(data.timestamp * 1000);
                tsEl.innerText = d.toLocaleTimeString();

                if (priceHistory[sym].length > 1) {
                    const min = Math.min(...priceHistory[sym]);
                    const max = Math.max(...priceHistory[sym]);
                    const range = max - min || 1;
                    
                    let path = "";
                    priceHistory[sym].forEach((p, i) => {
                        const x = (i / 29) * 100;
                        const y = 40 - (((p - min) / range) * 36 + 2); 
                        path += `${i===0?'M':'L'}${x},${y} `;
                    });
                    sparkEl.setAttribute('d', path);
                    sparkEl.setAttribute('stroke', strokeColor);
                }

                cardEl.classList.remove('glow-up', 'glow-down');
                void cardEl.offsetWidth; 
                cardEl.classList.add(glowClass);
                setTimeout(() => cardEl.classList.remove(glowClass), 600);

                const ticks = document.querySelectorAll(`#tick-${sym}`);
                ticks.forEach(t => {
                    t.innerHTML = `${sym} <span class="${colorClass}">${data.price.toFixed(2)}</span>`;
                    t.classList.add(isUp ? 'flash-up' : 'flash-down');
                    setTimeout(() => t.classList.remove('flash-up', 'flash-down'), 300);
                });
            });

            if (selectedSymbol === sym && currentTab === 'history') {
                fetchHistory(sym);
            }
        }

        function connect() {
            if (ws) {
                ws.close();
            }
            const token = document.getElementById('token-input').value;
            ws = new WebSocket(`wss://${window.location.hostname}:8443/?token=${token}`);

            ws.onopen = () => {
                isConnected = true;
                document.getElementById('status-dot').classList.add('connected');
                reconnectDelay = 1000;
            };

            ws.onmessage = (e) => {
                msgCount++;
                const now = Date.now();
                if (lastMsgTime > 0) {
                    document.getElementById('latency-display').innerText = (now - lastMsgTime) + 'ms';
                }
                lastMsgTime = now;
                
                try {
                    const data = JSON.parse(e.data);
                    if (data.type === 'price') {
                        updateCard(data);
                    }
                } catch(err) {}
            };

            ws.onclose = () => {
                isConnected = false;
                document.getElementById('status-dot').classList.remove('connected');
                document.getElementById('latency-display').innerText = '--ms';
                
                setTimeout(connect, reconnectDelay);
                reconnectDelay = Math.min(reconnectDelay * 2, 30000);
            };
        }

        document.getElementById('connect-btn').onclick = connect;

        let currentTab = 'history';

        window.switchTab = function(tab) {
            currentTab = tab;
            document.getElementById('tab-history').classList.remove('active');
            document.getElementById('tab-logs').classList.remove('active');
            document.getElementById('history-view').style.display = 'none';
            document.getElementById('logs-view').style.display = 'none';
            
            document.getElementById(`tab-${tab}`).classList.add('active');
            document.getElementById(`${tab}-view`).style.display = 'block';

            if (tab === 'logs') fetchLogs();
            if (tab === 'history' && selectedSymbol) fetchHistory(selectedSymbol);
        }

        window.selectSymbol = function(sym) {
            selectedSymbol = sym;
            document.getElementById('stat-selected').innerText = `Selected: ${sym}`;
            
            document.querySelectorAll('.card').forEach(c => c.style.borderColor = 'var(--border)');
            document.getElementById(`card-${sym}`).style.borderColor = 'var(--accent)';
            
            if (currentTab === 'history') fetchHistory(sym);
        }

        async function fetchHistory(sym) {
            try {
                const token = document.getElementById('token-input').value;
                const res = await fetch(`/api/history?symbol=${sym}`, {
                    headers: { 'Authorization': `Bearer ${token}` }
                });
                if (!res.ok) {
                    if (res.status === 401) {
                        document.getElementById('history-view').innerHTML = "<div style='color:var(--red)'>401 Unauthorized - Invalid Token</div>";
                    } else {
                        document.getElementById('history-view').innerText = "Error loading history.";
                    }
                    return;
                }
                const data = await res.json();
                
                let html = `<table><tr><th>Time</th><th>Price</th><th>Change</th></tr>`;
                data.forEach(r => {
                    const d = new Date(r.ts * 1000).toLocaleTimeString();
                    const color = r.change >= 0 ? 'var(--green)' : 'var(--red)';
                    html += `<tr><td>${d}</td><td>${r.price.toFixed(2)}</td><td style="color:${color}">${r.change > 0 ? '+':''}${r.change.toFixed(2)}</td></tr>`;
                });
                html += `</table>`;
                document.getElementById('history-view').innerHTML = html;
            } catch (e) {
                document.getElementById('history-view').innerText = "Error loading history.";
            }
        }

        async function fetchLogs() {
            try {
                const token = document.getElementById('token-input').value;
                const res = await fetch(`/api/logs`, {
                    headers: { 'Authorization': `Bearer ${token}` }
                });
                if (!res.ok) {
                    if (res.status === 401) {
                        document.getElementById('logs-view').innerHTML = "<div style='color:var(--red)'>401 Unauthorized - Invalid Token</div>";
                    } else {
                        document.getElementById('logs-view').innerText = "Error loading logs.";
                    }
                    return;
                }
                const data = await res.json();
                
                let html = "";
                data.forEach(r => {
                    const d = new Date(r.ts * 1000).toLocaleTimeString();
                    html += `<div class="log-entry log-${r.level}">[${d}] [${r.level}] ${r.message}</div>`;
                });
                document.getElementById('logs-view').innerHTML = html;
            } catch (e) {
                document.getElementById('logs-view').innerText = "Error loading logs.";
            }
        }

        setInterval(() => {
            if (currentTab === 'logs') fetchLogs();
        }, 5000);

        setInterval(() => {
            document.getElementById('stat-msgs').innerText = `Msgs: ${msgCount}`;
            
            const upSecs = Math.floor((Date.now() - startTime) / 1000);
            const h = String(Math.floor(upSecs / 3600)).padStart(2, '0');
            const m = String(Math.floor((upSecs % 3600) / 60)).padStart(2, '0');
            const s = String(upSecs % 60).padStart(2, '0');
            document.getElementById('stat-uptime').innerText = `Uptime: ${h}:${m}:${s}`;
            
            document.getElementById('stat-rate').innerText = `Rate: ${(msgCount / (upSecs || 1)).toFixed(1)}/s`;
        }, 1000);

        async function fetchInitialData() {
            for (const sym of symbols) {
                try {
                    const token = document.getElementById('token-input').value;
                    const res = await fetch(`/api/history?symbol=${sym}`, {
                        headers: { 'Authorization': `Bearer ${token}` }
                    });
                    if (res.ok) {
                        const data = await res.json();
                        if (data && data.length > 0) {
                            const latest = data[data.length - 1];
                            updateCard({
                                type: 'price',
                                symbol: sym,
                                price: latest.price,
                                change: latest.change,
                                timestamp: latest.ts
                            });
                        }
                    } else if (res.status === 401) {
                        document.getElementById(`price-${sym}`).innerText = '0.00';
                        document.getElementById(`change-${sym}`).innerHTML = `<span class="down">Auth Failed</span>`;
                    }
                } catch(e) {}
            }
        }

        initGrid();
        fetchInitialData();

        setInterval(() => {
            if (!isConnected) {
                fetchInitialData();
            }
        }, 5000);
    </script>
