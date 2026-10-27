Мод для GTA San Andreas 1.0 US, с которым сюжет можно пройти вдвоём по сети.

## Что нового

- **Сцены на движке игры.** Сцены миссий на движке игры (с чёрными полосами) напарник смотрит той же камерой, что и хост, и слышит их реплики. Если миссия переносит хоста в другое место прямо перед сценой, напарник оказывается там вместе с ним. Подсказки во время таких сцен, например советы парикмахера после стрижки, напарник тоже видит.
- **Лоурайдеры и танец на пляже вдвоём.** В конкурсе лоурайдеров («Cesar Vialpando») и в танце на пляже («Life's a Beach») напарник играет рядом с хостом под ту же музыку, и его очки идут в общий счёт. В машине напарника тоже сидит девушка, а камеры конкурса показывают его машину.
- **«Home Invasion» вдвоём.** Ящики в ограблении дома с Райдером может носить и грузить в фургон любой из двоих. Напарник надевает ту же балаклаву и крадётся, у него своя полоска шума, а задние двери фургона открыты у обоих.
- **Миссию может начать напарник.** Если напарник пришёл в маркер миссии первым, хост видит приглашение и по Y сразу оказывается рядом с ним. Миссия начинается, как только в маркере стоят оба.
- **Без чёрного экрана после миссии.** Сюжетный прогресс хоста напарник теперь загружает в маркере следующей миссии, где экран и так темнеет перед её началом. Раньше загрузка случалась в случайный момент после каждой пройденной миссии и выглядела как лаг или чёрный экран. Если миссий нет 10 минут, мир загрузится в спокойный момент, как раньше.
- **Общие навыки и статистика.** Навыки вождения машины, мотоцикла, велосипеда и самолёта общие: у обоих лучший из двух. Статистика в меню паузы тоже общая. Подработка или испытание, пройденные одним игроком, засчитываются обоим вместе с наградой.
- **Своя война за территорию.** Напарник дальше 100 м от хоста может начать свою войну за район. Хост увидит в чате, за какой район она идёт и чем кончилась.
- **Транспорт для напарника.** Если в миссии хост сел на велосипед, мотоцикл или в машину без свободного места, рядом с напарником появляется такой же транспорт. Если миссия посадила хоста в самолёт прямо в воздухе, как в конце «Freefall», напарник садится к нему пассажиром.
- **Пассажир с геймпада.** Чтобы сесть пассажиром с геймпада, удерживайте треугольник полсекунды. На клавиатуре так же работает удержание клавиши входа.

## Исправления

- Убраны подвисания на 1–2 секунды. Их давал голосовой чат, даже когда никто не говорил, а ещё копия второго игрока, которая при каждом переодевании читала одежду с диска.
- Тело, которое убрала игра его владельца, больше не появляется снова и не роняет деньги раз за разом. Эта ошибка появилась в 0.3.0.
- После «The Green Sabre» напарник больше не получает 4 неснимаемые звезды в других городах, а заграждения между городами пропадают у него, как только он загрузит мир хоста в маркере следующей миссии.
- Напарник видит красные маркеры назначения из миссий хоста. Раньше он не видел их вовсе.
- Кассиры, продавцы Ammu-Nation, парикмахеры и татуировщики больше не появляются дважды.
- Пассажир, который высунулся из окна с оружием не из списка (не узи, Tec-9 или MP5), больше не стреляет «из пальцев».
- Клавиша T внутри чит-кода больше не открывает чат.
- Машины второго игрока больше не пропадают, когда миссия хоста расчищает место вокруг себя.
- Курсор в окне SA:COOP с кодом приглашения в полноэкранной игре больше не замирает.
- Если игра стоит в Program Files, сервер больше не теряет профили напарников: тогда его данные лежат в `%LOCALAPPDATA%\SA-COOP`.
- Если код приглашения не может связаться с ntfy.sh (в некоторых сетях этот сайт недоступен), игра так и пишет и советует VPN или открытый порт, а не винит брандмауэр.
- Напарник получает звёзды розыска по правилам миссии, как и хост. Раньше в некоторых миссиях он получал звёзды там, где хост их не получает.
- Копия второго игрока вздрагивает от ударов, как он сам.
- Машина второго игрока больше не выглядит тормозящей, со стоп-сигналами и дымом из-под задних колёс.
- Снимок фотоаппаратом больше не роняет игру в оконном режиме.
- В доме из «Home Invasion» у напарника больше нет посторонних людей.
- Граффити и собранные предметы у перезашедшего напарника такие же, как у хоста.
- Название района в сообщениях о войне банд больше не превращается в набор знаков.
- Если игра надолго подвисла, в `coop.log` теперь видно, на чём.

## Установка

Распакуйте `SA-COOP.zip` в папку игры, чтобы `coop.asi`, `install.bat` и папка `coop` оказались рядом с `gta_sa.exe`, и запустите `install.bat`. Подробная инструкция лежит в архиве в файле `Инструкция.txt`, а описание мода в README репозитория.

Чтобы обновиться с прошлой версии, распакуйте архив поверх старых файлов. Настройки и прогресс напарника останутся, а `install.bat` запускать второй раз не нужно.

Версии 0.3.0 и 0.3.1 друг к другу не подключаются, поэтому обновиться нужно и хосту, и напарнику.

## In English

A mod for GTA San Andreas 1.0 US that lets two players go through the story together over the network.

### What's new

- **In-game scenes.** The partner watches the missions' in-game scenes (with black bars) through the same camera as the host and hears their lines. If the mission moves the host somewhere else right before a scene, the partner ends up there too. The partner also sees the help texts during such scenes, for example the barber's tips after a haircut.
- **Lowriders and the beach dance for two.** In the lowrider contest (Cesar Vialpando) and the beach dance (Life's a Beach) the partner plays next to the host to the same music, and their points go to the shared score. A girl sits in the partner's car too, and the contest's cameras show the partner's car.
- **Home Invasion for two.** Either player can carry the crates of Ryder's house robbery and load them into the van. The partner wears the same balaclava and sneaks, has their own noise bar, and the van's rear doors are open for both.
- **The partner can start a mission.** If the partner reaches a mission marker first, the host gets an invitation and presses Y to be there at once. The mission begins as soon as both stand in the marker.
- **No black screen after a mission.** The partner now loads the host's story progress in the next mission's marker, where the screen goes dark before the mission anyway. Before, the load came at a random moment after every passed mission and looked like lag or a black screen. With no mission for 10 minutes, the world loads at a quiet moment as before.
- **Shared skills and stats.** Car, motorbike, bicycle and flying skills are shared: both players have the better of the two. The pause menu stats are shared too. An odd job or challenge one player completes counts for both, together with its reward.
- **A gang war of your own.** A partner more than 100 m from the host can start their own war for a district. The host sees in the chat which district it is for and how it ended.
- **A vehicle for the partner.** If the host gets on a bicycle or a motorbike, or into a car with no free seat during a mission, the same vehicle appears next to the partner. If the mission puts the host in a plane in mid-air, as at the end of Freefall, the partner gets in as a passenger.
- **Passenger with a gamepad.** To get in as a passenger with a gamepad, hold triangle for half a second. Holding the enter key does the same on a keyboard.

### Fixes

- No more 1-2 second freezes. They came from the voice chat, even with nobody talking, and from the other player's copy reading its clothes from disk on every change.
- A body removed by its owner's game no longer comes back and drops money over and over. This bug came with 0.3.0.
- After The Green Sabre the partner no longer gets 4 wanted stars that never fade in the other cities, and the roadblocks between cities go away for them once they load the host's world in the next mission's marker.
- The partner sees the red destination markers of the host's missions. Before, the partner saw none of them.
- Cashiers, Ammu-Nation clerks, barbers and tattoo artists no longer appear twice.
- A passenger leaning out with a weapon not on the list (not an Uzi, Tec-9 or MP5) no longer shoots "from their fingers".
- The T key in the middle of a cheat code no longer opens the chat.
- The other player's cars no longer vanish when the host's mission clears the area around it.
- The cursor in the SA:COOP window with the invite code no longer freezes in a fullscreen game.
- With the game in Program Files, the server no longer loses the partners' profiles: its data then goes to `%LOCALAPPDATA%\SA-COOP`.
- When the invite code cannot reach ntfy.sh (some networks block it), the game says so and suggests a VPN or an open port instead of blaming the firewall.
- The partner gets wanted stars by the mission's rules, like the host. Before, in some missions the partner got stars where the host gets none.
- The other player's copy flinches from hits the way they do.
- The other player's car no longer looks like it is braking, with brake lights and smoke from the rear wheels.
- Taking a photo with the camera no longer crashes the game in windowed mode.
- The partner no longer finds strangers in the house of Home Invasion.
- Tags and collected items look the same for a partner who rejoins as for the host.
- District names in gang war messages no longer turn into gibberish.
- When the game freezes for long, `coop.log` now shows what it was busy with.

### Installation

Unpack `SA-COOP.zip` into the game folder so that `coop.asi`, `install.bat` and the `coop` folder sit next to `gta_sa.exe`, and run `install.bat`. The archive has detailed instructions in `Инструкция.txt` (Russian), and the repository's README describes the mod.

To update from the previous version, unpack the archive over the old files. The settings and the partner's progress stay, and `install.bat` does not need to run again.

Versions 0.3.0 and 0.3.1 cannot connect to each other, so both the host and the partner need to update.
