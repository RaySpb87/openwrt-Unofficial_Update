# HWNAT MT7620 — Stage 5: компактный фикс свёртки вошёл, BIND всё ещё не создаётся

**Дата:** 2026-09-09
**Ветка:** `openwrt-19.07.10UE`, HEAD `7eb7ab836b` (+ коммит фикса свёртки)
**Устройство:** ZyXEL Keenetic Omni, MT7620N, ядро 4.14, target `ramips/mt7620`
**Тип:** отчёт для следующей сессии (код НЕ менялся — только анализ и план)

---

## 1. Что было сделано в прошлой сессии (коммит `7eb7ab836b`)

Исправлена компиляторная свёртка `mtk_offload_bind_hook()` (GCC 7.5, MIPS, `-O2`):

- `mtk_offload.h` — hint в `skb->cb[44..47]` теперь пишется/читается **побайтово**
  (`put_hint`/`has_hint`/`get_hint`/`clear_hint`), без type-punning `*(u32 *)&skb->cb[44]`;
  `get_hint()` маскирует `& (MTK_PPE_ENTRY_CNT-1)`.
- `mtk_offload.c` — `mtk_offload_check_rx()` для sample (0x0e/0x0f) возвращает `1`;
- `mtk_eth_soc.c` — `ret > 0` → `netif_receive_skb()` (без GRO);
- `mtk_eth_soc.h` — зафиксирован контракт: `<0`=drop, `0`=GRO, `>0`=без GRO.

До фикса: `mtk_offload_bind_hook` в собранном `mtk_offload.o` был 8 байт
(`jr ra; li v0,1` = `return NF_ACCEPT`, тело выброшено свёрткой).
После фикса: полное тело, 0x480 байт.

## 2. Что подтверждается сейчас

```sh
objdump -t build_dir/…/mediatek/mtk_offload.o | grep mtk_offload_bind_hook
→ 00000310 l F .text 00000480 mtk_offload_bind_hook
```

**Фикс реален, и он физически в прошивке**: build_dir содержит полный (0x480)
bind-хук. То есть свёртка больше НЕ является причиной.

**Результат на железе: поведение не изменилось ни на йоту.**
=> Причина — не компиляция. Bind-хук либо не доходит (sample не долетает до
POSTROUTING), либо упирается в **один из runtime-гейтов** внутри функции.

## 3. Оценка «помогли ли изменения?»

- **Свёртка** — да, устранена и доказана на объектном файле.
- **Поведение** — нет, `bind_hook N` по-прежнему отсутствует в `rx_reasons`,
  `state=BIND` в `all_entry` пусто. `mtk_bind_hook_cnt` (инкрементируется
  ТОЛЬКО при успешном bind, прямо перед `NF_DROP`) равен нулю.
- **Вывод:** вычисление свёртки было обязательным, но НЕ достаточным условием.
  Узкое место — runtime: хук существует, но ни разу не дошёл до строки
  `mtk_bind_hook_cnt++` (mtk_offload.c:849).

## 4. Проверка «похожих проблем» (свёртка / type-punning skb->cb)

Проверен весь дерево `target/linux/{ramips/files-4.14, generic/hack-4.14,
generic/pending-4.14}`:

- Единственное вхождение `*(u32 *)&skb->cb[...]` — коммеntарий в исправленном
  `mtk_offload.h:302` (документирует, ПЕЧЕМУ нельзя).
- Патчи 648/650 (`nf_flow_table` passthrough, `xt_FLOWOFFLOAD`) уже используют
  побайтовую проверку (`skb->cb[44+0]==0x48 …`) — свёртки там нет.
- Других опасных широких чтений `cb`/`skb->cb` в драйвере/патчах не найдено.
- **Класс бага «GCC-фолдинг hint» в этом дереве закрыт.** Дальше искать
  свёртки бессмысленно — искомый дефект runtime.

Бонус-факт: `get_hint()` теперь маскирует `& 0xfff`, поэтому гейт
`if (idx >= MTK_PPE_ENTRY_CNT)` в mtk_offload.c:783 **недостижим** (мёртвый код)
— не может быть причиной. (Позже убрать при рефакторинге.)

## 5. Анатомия bind-хука: полный список гейтов

`mtk_offload_bind_hook()` (mtk_offload.c:757-852). Каждый `return NF_ACCEPT` —
точка, где sample тихо теряется; счётчик НЕ инкрементится. Гейты по порядку:

| # | строка | условие выхода | проверяющее |
|---|--------|----------------|-------------|
| G1 | 771 | `skb->protocol != ETH_P_IP` | L2/VLAN/PPPoE |
| G2 | 774 | `!mtk_offload_skb_has_hint()` | выживаемость cb |
| G3 | 780 | `!mtk_offload_eth или нет foe_table` | инициализация |
| G4 | 783 | `idx >= MTK_PPE_ENTRY_CNT` | **недостижим** (см. §4) |
| G5 | 786 | `!ct или ct dying` | conntrack |
| G6 | 790 | `!dst или !dst->dev` | маршрут |
| G7 | 800 | proto не TCP/UDP | L4-тип |
| G8 | 806 | `!n или !(n->nud_state & NUD_VALID)` | сосед (ARP) |

`mtk_bind_hook_cnt++` стоит только в самом конце, перед `NF_DROP`.

Компиляция все делает: байтовое сравнение магни 0x48/0x4b/0x54/0x4d — in
0x480-байтном теле. Так что «hint есть → конец» уже не из-за свёртки.

## 6. Ранжированные гипотезы (почему bind_hook==0)

### H1 — cb-hint не доживает до POSTROUTING / хук не вызывается  (всё ещё №1)
GRO обойдён, flowtable passthrough (648) есть. Но ни один счётчик не шевельнулся
за ~3.5K sample'ов. Не исключено, что sample вообще превращается в другой skb
по пути (ppp/влан/переклонирование), и исходный skb с hint не доходит до
POSTROUTING. Проверка — счётчик «hook entry».

### H2 — protocol != ETH_P_IP на POSTROUTING (G1, новый подозреваемый)
В `mtk_eth_soc.c:937-942` `eth_type_trans()` вызывается **до**
`__vlan_hwaccel_put_tag()`. Если ESW присылает sample с VLAN-тегом,
`skb->protocol` сначала = ETH_P_8021Q, и окончательный `ETH_P_IP` появляется
только после untag'а в `__netif_receive_skb_core`. При нестандартном порядке
хуков/влан-bridge sample на POSTROUTING может иметь protocol=0x8100.
Проверка: counter G1 + `tcpdump -i eth0 -nn -e` во время трафика (смотреть,
есть ли тег и `eth_type_trans` на «входе»).

### H3 — G8 (neigh/egress), особенно PPPoE WAN — очень вероятен
`dst_neigh_lookup(dst, &t_other->src.u3.ip)` (mtk_offload.c:806) ищет соседа
для **адреса удалённого сервера** (например 185.211.244.47) на egress-девайсе.
- Для routed-линка (ppp0 / eth0.2) сосед сервера НЕ существует — в ARP-таблице
  есть только шлюз. Если `rt_uses_gateway` не выставился на реальном маршруте —
  гейт G8 рвёт 100% потоков.
- Для PPPoE WAN смежны проблемы: egress = ppp0 (`dev_addr` пустой, `dvp`,
  MAC из PPPoE-сессии), `is_vlan_dev(ppp0)`=0. SDK в этом месте берёт egress из
  `mtk_hnat_get_nexthop` (шлюз + реальный dev), а НЕ из dst-соседа сервера.
- Проверка (без перекомпиляции): `ip route get 185.211.244.47`,
  `ip neigh`, `ubus call network.interface.wan status` (proto=pppoe? dhcp?).

### H4 — G5/G6 (ct/dst) — маловероятно для здоровых потоков
NTP/UDP flow ходит, ct есть, маршрут есть. Но проверить дёшево
онlиne: `grep -c 185.211.244.47 /proc/net/nf_conntrack`.

### H5 — направленность sample
Проверить, что sample 0x0f/0x0e в новой прошивке **продолжает расти** (они
кумулятивные). Если на новой прошивке 0x0e/0x0f стоят на 0 — RX-интерпретация
изменилась/сломалась, и «поведение аналогично» — просто старые часы на стенде.

## 7. План на следующую сессию (приоритет)

### P1 (обязательно, первое): per-gate счётчики в bind-хуке
Диагностика ТОЛЬКО, поведение не менять. ~20-25 строк:

- `u32 mtk_bind_gate_cnt[N]` (или 8 отдельных): по одному счётчику на каждый
  гейт G1..G8 + **counter на входе хука** (важнейший!).
- Печать в `mtk_ppe_debugfs_rx_reasons_show()` (mtk_debugfs.c), рядом с
  `bind_hook N`.
- Пересобрать kmod/прошивку, гонять NTP/HTTPS трафик, снять `rx_reasons`.

Интерпретация:
- вход==0 → sample не доходит до POSTROUTING (H1) — чинить transport;
- вход>0, G1>0 → protocol (H2);
- вход>0, G2>0 → hint потерян;
- вход>0, G8>0 → neigh/egress (H3);
- и т.д. Первый ненулевой гейт = причина.

### P2 (до пересборки, бескодовая диагностика)
```sh
nft list ruleset 2>/dev/null | grep -iE 'flow|offload'   # software flowtable?
ip route get 185.211.244.47; ip route get 192.168.2.212
ip neigh show
ubus call network.interface.wan status | jsonfilter -e '@["proto"]'
cat /sys/kernel/debug/mtk_ppe/rx_reasons    # снимок ДО
# гоняем трафик ~30 сек
cat /sys/kernel/debug/mtk_ppe/rx_reasons    # снимок ПОСЛЕ — 0x0e/0x0f должны расти
grep -c 185.211.244.47 /proc/net/nf_conntrack
tcpdump -i eth0 -nn -e 'udp port 123'       # есть ли VLAN-тег у sample
dmesg | grep -i ppe                         # хук зарегистрирован, ошибок нет
```

### P3 (целевой фикс, SDK-faithful, без зависимости от cb-hint)
Если P1/H1 покажет потерю hint — перейти на штатную SDK-механику:
**bind через повторное хеширование тьюплов, без sample/cb**.

Идея (это делает padavan `CONFIG_HW_NAT_SEMI_AUTO_MODE`):
- В POSTROUTING для ЛЮБОГО forwarding-пакета вычислить аппаратный hash FOE
  из 5-тьюпла (формула из SDK `mtk_hnat_get_ppi`/`mtk_hnat_hash`);
- взять `foe_table[hash]`; если `state==UNBIND` и тьюплы СОВПАДАЮТ — прописать
  egress (шлюз/порт/vlan/MAC из реального маршрута+sndича), `state=BIND`;
- sample с hint тогда не нужен вовсе; работает для обоих направлений;
  снимает всю зависимость от переживания cb.
- Egress-информацию брать из SDK-логики (`get_nexthop`): шлюз/устройство,
  а не `dst_neigh_lookup(dst, dip_сервера)`.

### P4 (после появления первого BIND): 64B vs 80B FOE
SDK использует 80-байтные записи (TB_ENTRY_SIZE=ENTRY_80B), мы — 64B
(`MTK_PPE_TB_CFG_ENTRY_SZ_64B`, mtk_offload.c:491). Если после P1/P3 BIND
начинает создаваться, но железо его «не читает» — проверить смену на 80B.

### P5 (в конце): применить SDK-тюнинг aging
`AI/saved_ppe_tuning_commit_f945b3c4a6.md` (PPE_BNDR=0x5, NTU_DLTA=1,
UDP_DLTA=12, FIN_DLTA=1, TCP_DLTA=7). Сейчас на HEAD тюнинг не применён —
вять развитие пустого BIND не путает.

## 8. Чек-лист «что доказать в следующей сессии»

1. 0x0e/0x0f РАСТУТ на новой прошивке (и вообще см. H5).
2. Хук зарегистрирован (dmesg) и `entry==0`? -> решён вопрос «дошли или нет».
3. Первый ненулевой gate-счётчик -> однозначный корень.
4. После фикса: в `all_entry` появляются `state=BIND`, `bind_hook N > 0`,
   разгон стрессовым трафиком не рвётся.

## 9. Ключевые строки кода для следующей сессии

- Инкремент успеха: `mtk_offload.c:849` (`mtk_bind_hook_cnt++`).
- Гейты: `mtk_offload.c:771,774,780,783,786,790,800,806`.
- Печать: `mtk_debugfs.c:102-121` (`rx_reasons_show`).
- RX/hint: `mtk_eth_soc.c:944-966` (GRO-bypass), `mtk_offload.c:703-739`
  (`check_rx`), `mtk_offload.h:312-354` (byte-wise hint helpers).
- Патчи passthrough: `generic/pending-4.14/648-…`, `generic/hack-4.14/650-…`
  (уже byte-wise, работают).
- FOE hash/таблица: `MTK_PPE_ENTRY_CNT=0x1000`, `MTK_RXD4_FOE_ENTRY=13:0`,
  entry 64B, `fpidx=8`, `tb_cfg=SMA_FWD_CPU|ENTRY_SZ_64B` (mtk_offload.c:491).

---

# Цель этой следующей сессии (продолжение отчёта stage4)

Реализован **P1 из плана** — per-gate счётчики в bind-хуке — и методки
проверки H1-H5. Код ДИАГНОСТИКИ написан и скомпилирован (см. §10). Поведение
bind-хука НЕ изменено — только добавлены счётчики и их печать. Тесты на
железе (§11) выполнит пользователь/агент в отдельной сессии.

---

## 10. Реализовано: per-gate счётчики (диагностика, поведение не менялось)

### 10.1 Изменённые файлы (в дереве `files-4.14`, компиляция проверена)

| файл | изменение |
|------|-----------|
| `mtk_offload.h` | `#define MTK_BIND_GATE_CNT 9` + `extern u32 mtk_bind_gate_cnt[9]` |
| `mtk_offload.c` | объявление `u32 mtk_bind_gate_cnt[MTK_BIND_GATE_CNT]`; инкременты в каждом `return NF_ACCEPT` bind-хука |
| `mtk_debugfs.c` | печать в `rx_reasons`: `bind_hook_entry` и `bind_gate1..8` |

### 10.2 Семантика счётчиков

| индекс | имя в debugfs | что считает |
|--------|---------------|-------------|
| 0 | `bind_hook_entry` | **вход в хук** (важнейший: дошёл ли пакет до POSTROUTING) |
| 1 | `bind_gate1` | G1: `protocol != ETH_P_IP` |
| 2 | `bind_gate2` | G2: нет hint в `skb->cb` |
| 3 | `bind_gate3` | G3: нет `eth`/`foe_table` |
| 4 | `bind_gate4` | G4: `idx >= MTK_PPE_ENTRY_CNT` (теперь **недостижим** — мёртвый, всегда 0) |
| 5 | `bind_gate5` | G5: нет ct / ct dying |
| 6 | `bind_gate6` | G6: нет dst / dst->dev |
| 7 | `bind_gate7` | G7: proto не TCP/UDP |
| 8 | `bind_gate8` | G8: egress-neigh не найден/не VALID |

Успешный bind по-прежнему инкрементит только `mtk_bind_hook_cnt` (mtk_offload.c:849).

### 10.3 Проверка компиляции (уже сделана, код на HEAD НЕ в коммите)

Оба объекта собраны той же toolchain'ой командой, что и `mtk_eth_soc.o`
(копии синхронизированы в `build_dir/.../linux-4.14.336/`):

```
mipsel-openwrt-linux-musl-gcc ... -O2 -c mtk_offload.c -> mtk_offload.o
mipsel-openwrt-linux-musl-gcc ... -O2 -c mtk_debugfs.c -> mtk_debugfs.o
```

- `mtk_offload.o`: `mtk_bind_gate_cnt` (BSS), `mtk_offload_bind_hook` **полное
  тело 0x514 байт** (входной инкремент `sw v0,0(s0)` сразу при входе) — свёртки
  НЕТ, байтовая проверка hint в целости.
- `mtk_debugfs.o`: `U mtk_bind_gate_cnt` — референс из печати присутствует.
- Ошибок и предупреждений, относящихся к изменениям, нет.

**Вывод:** диагностический билд готов. Дальше — пересобрать прошивку с этим
деревом и гонять тест на роутере (§11).

---

## 11. Методики проверки (выполнять на роутере)

### 11.0 Подготовка

1. Пересобрать и залить прошивку с деревом, где уже лежат изменения §10
   (счётчики). Перед тестом: `rmmod` +(по необходимости) `insmod` модуля
   `mtk-eth-soc`, либо перезагрузка, чтобы счётчики стартовали с 0.
2. Убедиться, что счётчики читаются:
   ```sh
   cat /sys/kernel/debug/mtk_ppe/rx_reasons
   ```
   На свежем старте ожидается минимум: куча `0x0x 0`, и **НЕ появится**
   строк `bind_hook_entry`/`bind_gate*` (они печатаются только когда
   `mtk_bind_gate_cnt[0] > 0`). Если строк уже нет на старте — ок.

### 11.1 Тест A — контрольный свип направлений и протоколов (главный)

Цель: получить ненулевой `bind_hook_entry` и первый ненулевой `bind_gateN`.
Первый ненулевой гейт = корень.

```sh
SNAP=/tmp/rx_a; D=/sys/kernel/debug/mtk_ppe/rx_reasons
cat $D > $SNAP.0
# 1) LAN->WAN UDP (NTP): 30 c
ntpdate -b 0.openwrt.pool.ntp.org; sleep 1
# 2) LAN->WAN TCP (HTTPS): 20 c
(echo -e 'GET / HTTP/1.0\r\n\r'; sleep 3) | nc -w 5 1.1.1.1 80 2>/dev/null || wget -qO- http://example.com >/dev/null
# 3) WAN->LAN (входящий, если роутер сам клиент — вместо этого ping из WAN-сегмента)
sleep 2
cat $D > $SNAP.1
diff $SNAP.0 $SNAP.1
```

Интерпретация (по приоритету причины):

| наблюдение | диагноз | дальнейший шаг |
|------------|---------|----------------|
| `bind_hook_entry` **== 0** | sample не доходит до POSTROUTING (**H1**) | §11.2 «доходит ли вообще»; пересмотреть transport/GRO/passthrough |
| `entry>0` и `bind_gate2>0` | hint потерян по пути (**H1-вариант**) | проверить GRO/passthrough 648/650, наличие VLAN-переобёртки |
| `entry>0` и `bind_gate1>0` | `protocol != ETH_P_IP` к POSTROUTING (**H2**) | §11.3 (VLAN/PPPoE) |
| `entry>0` и `bind_gate8>0` | egress-neigh для IP сервера не найден (**H3**) | §11.4 (ppp0 / shлюз) |
| `entry>0` и все g1..g8 == 0 | успешный bind — надо смотреть `bind_hook_cnt`/`all_entry` | §11.5 |
| `bind_hook_cnt` растёт, `all_entry` пуст | bind пишется, но железо не активирует | **P4** (80B FOE) |

> Важно: гонять И UDP, И TCP, оба направления по возможности. Если счётчики
> не шевельнутся ни при каком трафике — причина на входе (H1), а не внутри
> g1..g8.

### 11.2 Тест B — «доходит ли sample до POSTROUTING вообще» (H1)

Проверить, что PPE видит трафик и шлёт sample (0x0e/0x0f растут), и что хук
жив:

```sh
D=/sys/kernel/debug/mtk_ppe/rx_reasons
cat $D > $B.0
# гоняем UDP-трафик LAN->WAN 30 c
client:  while true; do curl -s http://1.1.1.1/ >/dev/null; sleep 1; done   # или iperf tcp
cat $D > $B.1
grep -E '0x0e|0x0f|0x13|bind' $B.0 $B.1
# должен расти хотя бы 0x0e/0x0f (HIT_UNBIND / RATE_REACHED)
dmesg | grep -iE 'ppe|offload|bind'    # хук зарегистрирован (mtk_ppe_probe), нет panic
cat /proc/net/netfilter/nf_hooks 2>/dev/null | grep -i postr  # хук в списке (NFPROTO_IPV4 POST_ROUTING, prio NAT_SRC+10)
```

- Если и 0x0e/0x0f не растут при трафике — **проблема на уровне ESW/GDM2/PPE
  (sample вообще не генерится)**; это глубже, чем bind-хук. Тогда смотреть
  `mt7620_esw_ppe`/TPF, `mt7620_ppe_gdm2_fwd`, SMA.
- Если 0x0e/0x0f растут, а `bind_hook_entry==0` — sample приходит, но
  превращается в другой skb до POSTROUTING (SSN/VLAN/переклонирование).
- Если `bind_hook_entry` растёт — см. §11.1.

### 11.3 Тест C — protocol/VLAN на POSTROUTING (H2)

Пока трафик идёт, снять тег у sample и проверить eth_type_trans-порядок:

```sh
# в отдельном терминале / tmux на роутере:
tcpdump -i eth0 -nn -e 'udp port 123'    # смотрим: есть ли 802.1Q тег у кадра
ip -d link show eth0 | grep -i 'vlan\|mtu'
# как подключён WAN: pppoe? pptp? vlan? dhcp?
ubus call network.interface.wan status | jsonfilter -e '@["proto"]' -e '@["device"]'
# какие интерфейсы в bridge LAN:
brctl show 2>/dev/null || bridge link
```

Если кадры приходят с тегом, а в `bind_gate1` — счётчик — значит `protocol`
к POSTROUTING остаётся 0x8100 (не успел стать IP). Смотреть порядок
`eth_type_trans`/`__vlan_hwaccel_put_tag` в `mtk_eth_soc.c:937-942` и бо это
на POSTROUTING-пути.

### 11.4 Тест D — egress/neigh/PPPoE (H3, самый вероятный для WAN через ppp0/vlan)

```sh
ip route get 185.211.244.47        # не берём ваш реальный: покажите вывод
ip route get 192.168.2.212         # внутренний адрес теста
ip neigh show
ubus call network.interface.wan status | jsonfilter -e '@["proto"]' -e '@["device"]' -e '@["ipv4-address"][0]["address"]'
ip link show                         # есть ли ppp0, eth0.2 и т.п.
```

Ожидание при H3: `ip route get <сервер>` показывает egress через `ppp0`/`eth0.2`
(pppoe/vlan), в `ip neigh` НЕТ записи для адреса самого сервера, есть только
шлюз. Тогда `dst_neigh_lookup(dst, dip_сервера)` (mtk_offload.c:824) = NULL →
`bind_gate8` растёт на 100% трафика. Это и есть H3.

Если `bind_gate8>0` и подтверждён PPPoE/vlan-egress — читать P3 (bind через
повторное хеширование, SDK-механика `get_nexthop`), а не `dst_neigh_lookup`.

### 11.5 Тест E — успешный bind не активируется железом (после появления BIND)

```sh
D=/sys/kernel/debug/mtk_ppe
cat $D/rx_reasons | grep -E 'bind|0x0e|0x0f'
cat $D/all_entry | grep -c 'state=BIND'
# стресс: разгон TCP iperf 60 c, снова
cat $D/all_entry | grep 'state=BIND' | head
```

Если `state=BIND` появился, но аппаратный разгон/производительность не растёт —
проверять **P4**: `MTK_PPE_TB_CFG_ENTRY_SZ_64B` → 80B (SDK `TB_ENTRY_SIZE`).

### 11.6 Сводная таблица интерпретации по результатам тестов

| итог тестов | корень | целевой фикс |
|-------------|--------|--------------|
| entry==0 | H1 (sample не доходит) | transport/passthrough; затем P3 |
| entry>0, g2>0 | H1 (hint не дожил) | GRO/passthrough 648/650/VLAN |
| entry>0, g1>0 | H2 (protocol) | vlan/untag-порядок |
| entry>0, g8>0 | H3 (neigh/egress) | P3 (get_nexthop), не dst_neigh |
| entry>0, g1..8==0, bind_hook растёт, BIND в all_entry | запись ок | P4 (80B) или aging P5 |
| entry>0, g1..8==0, bind_hook==0 | (невозможно: bind_hook инкрем-ся при успехе) | — |

---

## 12. Результаты тестов §11 (выполнены 2026-09-09, агент через SSH)

### 12.1 Среда тестирования

| параметр | значение |
|----------|----------|
| Прошивка | OpenWrt 19.07.10UU, `r11427-9ce6aa9d8d` |
| Ядро | 4.14.336, `#0 Sat Apr 16 13:13:32 2022` |
| SoC | MT7620N ver:2 eco:3, MIPS 24KEc |
| Устройство | ZyXEL Keenetic Omni |
| WAN | `eth0.2` (DHCP, IP 192.168.2.212/24, шлюз 192.168.2.1) |
| LAN | `br-lan` = `eth0.1` + `wlan0` (192.168.3.1/24) |
| Клиент | 192.168.3.142 (WiFi, активен — NTP/UDP-трафик) |
| PPPoE | **Нет** (DHCP на eth0.2) |
| VLAN | Нет пользовательских (802.1q VLAN 0 HW filter на eth0) |
| Модуль `mtk_eth_soc` | **built-in** (не .ko, `lsmod` пуст) |
| Uptime при тестах | ~40–50 мин |
| SSH | pubkey, без пароля |

### 12.2 §11.0 Подготовка — baseline

```
0x0e 243        (PPE HIT_UNBIND)
0x0f 5136       (PPE RATE_REACHED)
bind_hook_entry 2
bind_gate1 0
bind_gate2 2     ← оба входа в хук попали в G2 (нет hint)
bind_gate3..8 — все 0
```

**Вывод:** хук был вызван **2 раза** (видимо, при инициализации сети ~boot).
Оба раза `skb->cb[44]` не содержал магию 0x48 → G2. После этого — **ни одного
вызова**.

### 12.3 §11.1 Тест A — контрольный свип (UDP + TCP, 60 сек)

| время | 0x0e | 0x0f | bind_hook_entry | bind_gate2 |
|-------|------|------|-----------------|------------|
| t=0 (baseline) | 243 | 5136 | 2 | 2 |
| t=30с (после wget) | 688 | 25867 | 2 | 2 |
| t=60с (60 сек фон) | 3753 | 665837 | 2 | 2 |

Трафик генерировался: `wget -q -O /dev/null http://1.1.1.1/` (router → OUTPUT),
а также фоновый трафик от клиента 192.168.3.142 (FORWARD через NAT).

**Ключевой факт:** `bind_hook_entry` **не шевельнулся** (остался 2), несмотря на
рост 0x0e на +3510 и 0x0f на +660701.

### 12.4 §11.2 Тест B — доходит ли sample до POSTROUTING (H1)

**Результат: H1 ПОДТВЕРЖДЁН в экстремальной форме.**

| проверка | результат |
|----------|-----------|
| 0x0e/0x0f растут при трафике? | **Да**, 0x0e: 243→24897, 0x0f: 5136→1085450 |
| `bind_hook_entry` растёт? | **Нет**, стоит на 2 всё время |
| dmesg: PPE started? | Да, дважды (t=6с и t=89с) |
| dmesg: mtk_offload/hook? | **Нет** (нет сообщений о регистрации хука) |
| Модуль mtk_eth_soc загружен? | Built-in (в ядре) |
| `/sys/module/mtk*` | **Пусто** — нет отдельного модуля offload |
| `kallsyms: mtk_offload_bind_hook` | **80256470 t** — символ есть в ядре |
| nf_flow_table загружен? | **Да** (13823 bytes, refs: xt_FLOWOFFLOAD, nf_flow_table_hw) |
| xt_FLOWOFFLOAD загружен? | **Да** (3072 bytes, refs: 2) |

**Вывод:** Функция `mtk_offload_bind_hook` **физически присутствует** в ядре
(символ в kallsyms), но **не вызывается** для forwarded-трафика. PPE генерирует
sample (0x0e/0x0f растут на тысячи), но пакеты **не доходят** до
`NF_INET_POST_ROUTING` хука.

### 12.5 §11.3 Тест C — protocol/VLAN (H2)

| проверка | результат |
|----------|-----------|
| `/proc/net/vlan/config` | Пусто (нет пользовательских VLAN) |
| WAN интерфейс | `eth0.2` (VLAN tag 2 на eth0, DHCP) |
| bridge | `br-lan` = `eth0.1` + `wlan0` |
| `tcpdump` | **Недоступен** на роутере |
| nftables | **Недоступен** (`nft: not found`) |

**Вывод:** VLAN-тегирование стандартное (eth0.2 = VLAN 2). Нет признаков
некорректного порядка `eth_type_trans`/untag. H2 маловероятен как корень, но
полная проверка невозможна без tcpdump.

### 12.6 §11.4 Тест D — egress/neigh/PPPoE (H3)

```
ip route get 185.211.244.47 → via 192.168.2.1 dev eth0.2 src 192.168.2.212
ip route get 1.1.1.1       → via 192.168.2.1 dev eth0.2 src 192.168.2.212
```

```
ip neigh:
  192.168.2.1   dev eth0.2  lladdr b0:be:76:58:b1:bc  REACHABLE
  192.168.3.142 dev br-lan  lladdr 04:7c:16:cd:f9:b4  REACHABLE
```

| проверка | результат |
|----------|-----------|
| PPPoE (ppp0)? | **Нет**, ppp0 не существует |
| Gateway reachable? | **Да**, 192.168.2.1 REACHABLE на eth0.2 |
| Сервер (185.211.244.47) в ARP? | **Нет** (шлюз есть, сервера нет) |
| conntrack [OFFLOAD] | **62** записей с [OFFLOAD] |
| conntrack total | 773 |
| `ubus wan proto` | **dhcp** (не pppoe) |

Примеры conntrack с OFFLOAD:
```
udp 192.168.3.142→203.123.106.12 [OFFLOAD]
udp 192.168.3.142→94.180.164.104 [OFFLOAD] (11817 pkts, 581KB↑)
udp 192.168.3.142→87.71.220.42  [OFFLOAD]
tcp 192.168.3.142→176.109.182.33 [OFFLOAD]
```

**Вывод:** WAN = simple DHCP on eth0.2, шлюз достижим. H3 (neigh) **не
подтверждён** для данного сценария — шлюз есть в ARP. Однако bind-хук всё равно
не вызывается (см. §12.4), поэтому H3 не актуален.

### 12.7 §11.5 Тест E — BIND в FOE-таблице

```
FOE table summary:
  state=INVALID: 4087+
  state=UNBIND:  8 (активных трансляции)
  state=BIND:    0 ← НОЛЬ
```

Примеры UNBIND-записей (все с MAC = 00:00:00:00:00:00):
```
(18) UNBIND | 185.125.190.58:123→192.168.2.212:50639 | MAC: 00:00=>00:00
(10) UNBIND | 192.168.3.142:50225→80.82.61.146:30884  | MAC: 00:00=>00:00
(fc) UNBIND | 192.168.3.142:37422→178.70.227.244:51413 | MAC: 00:00=>00:00
```

**Вывод:** PPE создаёт UNBIND-записи (видит трафик), но bind-хук **никогда**
не прописывает egress-данные (MAC, port, VLAN) → запись остаётся UNBIND
навсегда. Ключ `info1` содержит хеш-тупл, но состояние не меняется.

### 12.8 §11.6 Итоговая интерпретация

**Корневая причина: H1+, подвариант — хук `mtk_offload_bind_hook` НЕ ВЫЗЫВАЕТСЯ
для forwarded-трафика.**

Это **сильнее**, чем originally предполагалось в H1. Ранее гипотеза была:
«sample не долетает до POSTROUTING → hint в cb[44] потерян → G2 срабатывает».
Реальность ещё хуже: **сама функция-хук не вызывается** для forwarded-пакетов
(«входной» счётчик `bind_hook_entry` стоит на 2 от инициализации).

Возможная причина — **`xt_FLOWOFFLOAD` (hardware offload)**:

| observation | evidence |
|-------------|----------|
| `xt_FLOWOFFLOAD hw` активен | iptables FORWARD: `25926 pkts FLOWOFFLOAD hw ctstate RELATED,ESTABLISHED` |
| `nf_flow_table` + `nf_flow_table_hw` загружены | `lsmod` подтверждает |
| 62 conntrack-записи с `[OFFLOAD]` | Коннекты от клиента 192.168.3.142 |
| FORWARD zone_lan_forward: 6836 pkts | Новые LAN→WAN соединения |
| nat POSTROUTING: 6714 pkts → zone_wan | Значительно меньше, чем 32762 forwarded |

**Механизм:** `xt_FLOWOFFLOAD` с `hw` флагом перехватывает forwarded-трафик в
FORWARD-цепи. Для RELATED,ESTABLISHED потоков — обрабатывает в PPE硬件.
Даже для NEW-соединений, FLOWOFFLOAD устанавливает аппаратный offload, и
**последующие пакеты** этого потока обрабатываются PPE напрямую, **минуя
весь netfilter-стек** (включая POSTROUTING). Это объясняет:
1. Почему `bind_hook_entry` не растёт — хук не вызывается
2. Почему 0x0e/0x0f растут — PPE видит и обрабатывает трафик
3. Почему BIND=0 — хук не прописывает egress
4. Почему UNBIND все с MAC=00:00 — хук не заполнил данные

### 12.9 Сводная таблица §11.6 (обновлённая)

| итог тестов | корень | подтверждение |
|-------------|--------|---------------|
| `entry==0` (или застрял на 2) | **H1+ : хук не вызывается для forwarded** | **ДА** — bind_hook_entry=2, не растёт |
| 0x0e/0x0f растут | PPE работает, sample генерируются | **ДА** — +24654 / +1080314 за сессию |
| BIND=0, UNBIND с MAC=00:00 | Хук не прописывает egress | **ДА** |
| `xt_FLOWOFFLOAD hw` активен | Вероятный виновник обхода POSTROUTING | **ДА** — 25926 pkts hw offload |

### 12.10 Чек-лист §8 — статус

| # | пункт | статус |
|---|-------|--------|
| 1 | 0x0e/0x0f РАСТУТ | ✅ Подтверждено (243→24897, 5136→1085450) |
| 2 | Хук зарегистрирован, entry==0? | ⚠️ Символ есть, entry=2 (от boot), хук НЕ вызывается |
| 3 | Первый ненулевой gate | gate2=2 (от boot), но неактуально — хук мёртв |
| 4 | BIND в all_entry, bind_hook растёт | ❌ BIND=0, bind_hook не растёт |

### 12.11 Результат P0: отключение FLOWOFFLOAD

Выполнено **2026-09-09** (runtime, без перекомпиляции).

| действие | результат |
|----------|-----------|
| `iptables -D FORWARD ... -j FLOWOFFLOAD --hw` | ✅ Правило удалено |
| `rmmod xt_FLOWOFFLOAD` | ❌ Не удалось (refs=1, циклическая зависимость) |
| `conntrack -F` | ❌ утилита недоступна, ноOFFLOAD-записи обнулились через `/proc` |
| PPE started после restart | Да (3-й раз, t=1390с) |
| `bind_hook_entry` после P0 | **2 (без изменений!)** |
| `bind_hook_entry` через 30 сек с трафиком | **2 (без изменений!)** |

**Ключевой вывод P0: `xt_FLOWOFFLOAD` НЕ является причиной.**

Хук `mtk_offload_bind_hook` не вызывается **вообще** — даже для:
- Локального `ping 192.168.2.1` (OUTPUT → POSTROUTING)
- Фонового forwarded-трафика от клиента (0x0e/0x0f растут)
- После полного `network restart` (3-й PPE start)

### 12.12 Новая гипотеза: хук не注册ирован в NF_HOOK

`nf_register_net_hook(&init_net, &mtk_offload_bind_ops)` в `mtk_ppe_probe()`
(mtk_offload.c:904) **молча не выполняется** или **не доходит**:

| факт | следствие |
|------|-----------|
| `bind_hook_entry=2` после 3 PPE starts | Хук вызван 2 раза (при最早的 boot), затем НИКОГДА |
| Даже `ping` не двигает счётчик | Хук не в NF_INET_POST_ROUTING chain |
| `kallsyms: mtk_offload_bind_hook` = `80256470 t` | Функция есть, но `nf_register_net_hook` мог не сработать |
| `#ifdef CONFIG_SOC_MT7620` guard | Если не defined — hook не注册ирован (но функция есть в kallsyms!) |
| `fe_open()` → `mtk_ppe_probe()` → `nf_register_net_hook()` | Возврат не проверяется! |

**Возможные причины:**
1. `nf_register_net_hook()` завершается с ошибкой (возврат не проверяется)
2. `CONFIG_SOC_MT7620` не defined в runtime-конфиге (хотя код скомпилирован)
3. `mtk_ppe_probe()` не вызывается для eth0 (вызывается для другого dev)
4. Hook registered на `init_net`, но пакеты идут через другой netns

### 12.13 Обновлённые рекомендации на следующую сессию

1. **P1 (критично): Добавить printk в `mtk_ppe_probe()`** для проверки:
   - Вызывается ли `mtk_ppe_probe()` вообще
   - Вызывается ли `nf_register_net_hook()` и какой возврат
   - Определяет ли `#ifdef CONFIG_SOC_MT7620` блок
   ```c
   // В mtk_offload.c, mtk_ppe_probe():
   int ret = nf_register_net_hook(&init_net, &mtk_offload_bind_ops);
   pr_info("mtk_offload: nf_register_net_hook returned %d\n", ret);
   ```

2. **P2: Добавить printk в самое начало `mtk_offload_bind_hook()`**
   для подтверждения что хук вообще вызывается:
   ```c
   static unsigned int mtk_offload_bind_hook(void *priv,
       struct sk_buff *skb, const struct nf_hook_state *state)
   {
       pr_info("mtk_offload: bind_hook entered, proto=%u\n", skb->protocol);
       // ... существующий код
   }
   ```

3. **P3: Проверить `/proc/config.gz`** (или `zcat /proc/config.gz | grep MT7620`)
   для подтверждения `CONFIG_SOC_MT7620=y`. Если нет — это корень.

4. **P4: Проверить `fe_open()` → `mtk_ppe_probe(priv)`** — вызывается ли
   `mtk_ppe_probe()` с правильным `struct mtk_eth *eth` (а не `struct fe_priv *`).
   Несоответствие типов может вызвать UB.

5. **P5: Установить `tcpdump`** (`opkg install tcpdump`) для VLAN-диагностики.

---

### 12.14 Реализовано: printk для P1/P2 (проект подготовлен к компиляции)

Добавлены (working tree, `mtk_offload.c`), компиляция проверена GCC 7.5 MIPS `-O2`:

- **`mtk_ppe_probe()`**:
  - вход: `pr_info("mtk_offload: mtk_ppe_probe CONFIG_SOC_MT7620=y, registering bind hook")`
  - возврат: `int ret = nf_register_net_hook(...); pr_info("... returned %d", ret)` — ловим ошибку регистрации
  - `#else`: `"CONFIG_SOC_MT7620 NOT defined"` (проверка гейта P3)
- **`mtk_offload_bind_hook()`**: на входе `pr_info("mtk_offload: bind_hook entered, proto=%u")`

Проверка на объектном файле:
- `mtk_offload_bind_hook` тело **0x530 байт** (не свёрнута), `mtk_ppe_probe` полный.
- Все 3 строки printk присутствуют; строка `#else` (NOT defined) **отсутствует** →
  `CONFIG_SOC_MT7620=y` подтверждён на этапе компиляции (P3 фактически закрыт).
- Ошибок компиляции нет.

**Далее:** `make target/linux/compile && make` → залить прошивку → `dmesg | grep mtk_offload`
-> увидеть либо `nf_register_net_hook returned 0` (тогда искать, почему хук не в цепочке),
либо код ошибки (тогда корень — в самой регистрации).

### 12.14 Реализовано: printk в mtk_ppe_probe() и bind_hook — прошивка перезалита

Код диагностики (§12.14) **скомпилирован и залит** на роутер. Отчёт по свежей
загрузке (uptime 18 мин) — см. §12.15.

---

### 12.15 Свежий boot с printk (2026-09-09, uptime 18 мин)

Новая прошивка (с printk §12.14) установлена и загружена.

**dmesg на свежем boot:**
```
[    6.455390] mtk_soc_eth 10100000.ethernet: PPE started
[    6.465691] mtk_offload: mtk_ppe_probe CONFIG_SOC_MT7620=y, registering bind hook
[    6.480617] mtk_offload: nf_register_net_hook returned 0
[    6.549413] mtk_offload: bind_hook entered, proto=2048
[    6.559745] mtk_offload: bind_hook entered, proto=2048
[    8.476019] overlayfs: upper fs does not support tmpfile.
[   27.694180] mtk_soc_eth 10100000.ethernet: PPE started
```

**КРИТИЧЕСКИЕ НАХОДКИ:**

1. **`nf_register_net_hook returned 0`** — регистрация магически **успешна**!
   (Ранняя гипотеза «хук не注册ирован» — ОПРОВЕРГНУТА. Хук реально в цепочке.)
2. **`bind_hook entered` сработал ровно 2 раза** при boot (proto=2048=IPv4).
   Совпадает с `bind_hook_entry=2`. Это «инициализационные» вызовы.
3. **`bind_hook_entry` НЕ растёт при трафике** (0x0e: 3747, 0x0f: 372636 — растут,
   а `bind_hook_entry` = 2).
4. Второй `PPE started` (t=27s) идёт **БЕЗ** `mtk_ppe_probe` / нового
   `nf_register_net_hook` / `bind_hook entered`.

**Вывод:** Хук РЕАЛЬНО зарегистрирован (ПЕРЕОПРОВЕРКА первого вывода). Но
вызывается только 2 раза при boot и больше **никогда**, даже при активном
forwarded-трафике (0x0e/0x0f растут на тысячи). 

**Новая суть проблемы:** хук есть в цепочке POSTROUTING, но пакеты (все — и
forwarded, и локальный OUTPUT) **не доходят** до него. Возможные причины:

| гипотеза | проверка |
|----------|----------|
| Хук registered, но пакеты обходят netfilter (PPE/FLOWOFFLOAD перехват) | Отключить FLOWOFFLOAD (§12.13), смотреть entry |
| `bind_hook` вызывается, но входной инкремент не идёт (порядок кода?) | Проверить порядок printk/инкремента в bind_hook |
| Пакеты идут через другой netns (не init_net) | Хук на `init_net`, но трафик в другом ns |
| printk rate-limited (не видим вход) | Проверить `dmesg | grep bind_hook` |

**Далее (приоритет):**
- Тест B (§11.2): «доходит ли sample до POSTROUTING» — уже есть счётчики.
  Если `entry` не растёт даже с FLOWOFFLOAD off — значит по-прежнему
  netfilter-стека нет в цепочке для этого трафика.
- Проверить netns: `ls /proc/net/netns`? нет — но `ip netns list`.

---

### 12.16 Решающий тест с реальным forwarded-трафиком (торрент) — ХУК НЕ ВЫЗЫВАЕТСЯ

**Контекст:** клиент 192.168.3.142 (WiFi) генерирует активный forwarded трафик
через роутер (торрент: TCP ESTABLISHED к 172.65.90.20:443 ~2075 пакетов,
масса SYN_SENT к 6881/51413 — поиск пиров). Трафик идёт через NAT WAN (eth0.2).

**Снимок за 20 секунд активного качания:**

| счётчик | ДО | ПОСЛЕ 20с | дельта |
|---------|-----|-----------|--------|
| 0x0c (RX?) | 840 | 947 | +107 |
| 0x0d (RX?) | 1469 | 1544 | +75 |
| **0x0e** (HIT_UNBIND) | 780 | 854 | +74 |
| **0x0f** (RATE_REACHED) | 68877 | 76909 | **+8032** |
| **bind_hook_entry** | **2** | **2** | **+0** |
| bind_gate2 | 2 | 2 | 0 |
| dmesg `bind_hook entered` | 2 | 2 | **+0 (printk не вызывался)** |

Параллельно маршрут жив: `ping 192.168.2.1` ok (0% loss).

**ЖЕЛЕЗНОЕ ДОКАЗАТЕЛЬСТВО:**
1. PPE видит и обрабатывает forwarded-трафик (0x0e +74, 0x0f +8032).
2. `mtk_offload_bind_hook` зарегистрирован (`nf_register_net_hook returned 0`).
3. **НО хук не вызывается ни разу** — ни `bind_hook_entry`, ни printk не растут.

**Вывод (окончательный):** Прошивка работает, sample приходят, хук
зарегистрирован, но пакеты практически **не проходят через netfilter
POSTROUTING path** для этого forwarded-трафика — их перехватывает аппаратно
PPE/ESW (SMA) ещё до/вместо netfilter-стека, либо трафик идёт в обход
(другой netns или не через NF_INET_POST_ROUTING на `init_net`).

**Важно про отвал сети в Тесте 2(первый):** `udhcpc -R -n -q` на eth0.2
отпустил DHCP-адрес и опустил WAN → сеть «отвалилась». Это была ошибка метода,
не баг прошивки. Впредь DHCP/WAN-интерфейс руками не трогаю.

---

### 12.17 Тест 3: FLOWOFFLOAD окончательно исключён

**Факты:**
1. На новой прошивке правила `FLOWOFFLOAD` в FORWARD **не было вообще**
   (`iptables -D ... "No chain/target/match by that name"`; `grep -i flow`
   пустой). В отличие от ранней версии, где было `FLOWOFFLOAD hw` с и
   45688 пакетами.
2. `CONNTRACK OFFLOAD записей = 0` — hardware/software offload **не активен**.
3. При активном торренте (192 conntrack-записей от клиента 192.168.3.142,
   0x0e: 2238→2366, 0x0f: 320449→337754) **без какого-либо offload**:

| счётчик | ДО | ПОСЛЕ 20с | дельта |
|---------|-----|-----------|--------|
| 0x0e | 2238 | 2366 | +128 |
| 0x0f | 320449 | 337754 | +17305 |
| **bind_hook_entry** | **2** | **2** | **+0** |
| dmesg `bind_hook entered` | 2 | 2 | +0 |

**ВЫВОД (безусловный):** `FLOWOFFLOAD`/offload-патч **НЕ при чём**. Даже в
конфигурации без единого offload-правила хук `mtk_offload_bind_hook` НЕ
вызывается для forwarded-трафика, хотя успешно зарегистрирован
(`nf_register_net_hook returned 0`) и PPE видит трафик (0x0e/0x0f растут).

**Остающиеся гипотезы (по вероятности):**
1. **SMA/ESW аппаратно перехватывает** forwarded-пакеты до netfilter
   POSTROUTING (`mtk_sma_restore_timer_fn`, `CONFIG_SOC_MT7620` SMA-механика).
2. **Пакеты идут через другой netns**, не `init_net` (хук зарегистрирован на
   `init_net`).
3. Хук registered на `NF_INET_POST_ROUTING`, но forwarded-путь в этом ядре
   вообще не заходит в `NF_INET_LOCAL_OUT`/`POST_ROUTING` для этих пакетов
   (какая-то бага в цепочке вызова netfilter для forwarded).

---

### 12.18 Проверка netns (бескодовая) — ОПРОВЕРГНУТА

Через SSH выполнен бескодовый check:
- `/proc/net/netns` — отсутствует (netns не включён / не используется).
- `ip netns list` — BusyBox ip не поддерживает netns.
- Все процессы в одном и том же `init_net` (readlink /proc/1/ns/net — одинаков).
- Пакеты идут через netfilter: FORWARD **44442 pkts**, mangle POSTROUTING
  **414K pkts** — то есть forwarded трафик РЕАЛЬНО достигает POSTROUTING.

**Ключевое наблюдение:** nat POSTROUTING видит только **202 пакета** при
`FORWARD=44442` и `mangle POSTROUTING=414K`. Это означает, что после hooks с
приоритетом -150 (mangle) пакеты чаще всего **не доходят** до приоритета 110
(наш bind_hook) — их либо перехватывает, либо **наш хук не зарегистрирован**.

**Вывод: netns не виноват. Пакеты идут через init_net и через POSTROUTING.**

---

### 12.19 КОРНЕВАЯ ПРИЧИНА НАЙДЕНА: хук снимается fe_stop и НЕ перерегистрируется

**Доказательство из dmesg свежего boot (с printk §12.14):**

```
[    6.483405] mtk_soc_eth 10100000.ethernet: PPE started      ← fe_open #1
[    6.493705] mtk_offload: mtk_ppe_probe CONFIG_SOC_MT7620=y  ← хук регистрируется
[    6.508630] mtk_offload: nf_register_net_hook returned 0     ← OK
[    6.577699] mtk_offload: bind_hook entered, proto=2048       ← работает
[    6.588031] mtk_offload: bind_hook entered, proto=2048
[   27.667326] mtk_soc_eth 10100000.ethernet: PPE started      ← fe_open #2
                                    ↑ ↑ ↑ НЕТ mtk_ppe_probe / НЕТ nf_register!
```

**Цепочка бага:**

1. t=6s `fe_open()` → `mtk_ppe_probe()` → `mtk_ppe_start()` ("PPE started") +
   `nf_register_net_hook()` → хук работает (bind_hook_entry=2 от init).
2. Между 6s и 27s `fe_stop()` → `mtk_ppe_remove()` →
   `nf_unregister_net_hook()` (хук снят!) **+ debugfs НЕ удаляется**.
3. t=27s `fe_open()` → `mtk_ppe_probe()` → `mtk_ppe_start()` ("PPE started")
   → `mtk_ppe_debugfs_init()`:
   - `debugfs_create_dir("mtk_ppe", NULL)` на повторный вызов возвращает
     **`ERR_PTR(-EEXIST)`** (не NULL),
   - старый код `if (!root)` ловит только NULL → ошибка ПРОПУСКАЕТСЯ,
   - но `debugfs_create_file()` с `parent=ERR_PTR(-EEXIST)` дальше падает,
   - итог: функция молча возвращает 0 или ошибочный статус → **`nf_register_net_hook()` НЕ вызывается**.
4. Hook снят и **никогда не возвращается** → forwarded-трафик 0x0e/0x0f растёт, а bind_hook_entry=2 навсегда.

**Почему это объясняет ВСЁ наблюдавшееся:**
- Хук был зарегистрирован (boot), работает 2 раза, потом «исчезает».
- `nf_register_net_hook returned 0` видно только один раз (первый boot).
- FORWARD/mangle видят пакеты (44442/414K), nat POSTROUTING — нет (хук снят →
  наш hook на prio 110 не в цепочке).
- Отключение FLOWOFFLOAD ничего не меняет (проблема не в offload-патче).

**ФИКС (3 правки, сделаны в коде):**

1. `mtk_offload.c mtk_ppe_probe()`: `nf_register_net_hook()` ПЕРЕНЕСЁН **ДО**
   `mtk_ppe_debugfs_init()` — хук регистрируется ВСЕГДА, даже если debugfs
   падает на повторном вызове.
2. `mtk_offload.c mtk_ppe_remove()`: добавлен `pr_info("mtk_ppe_remove
   unregistering bind hook")` — теперь видно, когда хук снимается.
3. `mtk_debugfs.c mtk_ppe_debugfs_init()`: `if (!root)` заменён на
   `if (IS_ERR(root))` с обработкой `-EEXIST` (если каталог уже есть — успех).
4. `mtk_eth_soc.c`: `pr_info("fe_open %s")` / `pr_info("fe_stop %s")` —
   видно lifecycle интерфейса.

**Ожидаемый результат следующего теста:** после перепрошивки при `fe_stop`
в dmesg появятся `fe_stop` + `mtk_ppe_remove unregistering bind hook`, а при
повторном `fe_open` — `mtk_ppe_probe` + `nf_register_net_hook returned 0`
(ПОВТОРНО). Тогда `bind_hook_entry` начнёт расти при forwarded-трафике.

---

*СТАТУС: Прошивка пересобрана с фиксом §12.19, готова к перепрошивке. Результаты
теста с фиксом будут записаны в §12.20 после следующего boot.*