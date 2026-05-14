# dm-racewarn (Версия с list head)

`dm-racewarn` — внешний модуль ядра Linux для `device-mapper`, который
обнаруживает гонки на пересекающихся незавершённых I/O-запросах к блочному
устройству и пишет предупреждения в журнал ядра.

- Решение сделано на основе Linux kernel `6.8`.

- Тестировал на `6.8.0-100-generic` - Linux Mint 22.1. 

- Скачать исходники ядра можно так: 
``` bash
git clone --depth 1 --branch v6.8 https://github.com/torvalds/linux.git
```

## Ветки

В репозитории есть две ветки с одной и той же логикой target'а, но с разными
структурами данных для хранения активных интервалов:

- `dm-racewarn-ll` — реализация через `struct list_head` выполняется за `O(n)`, где `n` — количество `block io` запросов в списке
- `dm-racewarn-tree` — реализация через `interval_tree` - поиск пересечений эффективнее линейного обхода списка за счёт хранения активных диапазонов в интервальном дереве 

#### Эта ветка — `dm-racewarn-ll`, в ней используется `list_head`. 

## Получение исходников

Если нужна версия на `interval_tree`:

```bash
git clone --depth 1 --branch dm-racewarn-tree https://github.com/ArtemkaSheremetev/yadro_traid_task.git
```

Если нужна версия на `linked list`:

```bash
git clone --depth 1 --branch dm-racewarn-ll https://github.com/ArtemkaSheremetev/yadro_traid_task.git
```

## Состав решения

- `dm-racewarn.c` — код target'а `racewarn`
- `Makefile` — сборка внешнего модуля ядра
- `scripts/load-racewarn.sh` — загрузка модуля и проверка регистрации target'а
- `scripts/unload-racewarn.sh` — выгрузка модуля
- `tests/test-racewarn.sh` — smoke test

## Зависимости

Для сборки внешнего модуля нужны:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

## Сборка

Сборка выполняется в каталоге проекта:

```bash
make
```
Для очистки:

```bash
make clean
```

После сборки рядом с исходником появится:

```bash
dm-racewarn.ko
```

## Загрузка модуля

```bash
sudo bash scripts/load-racewarn.sh
```

Скрипт:

- загружает `dm-mod`
- загружает `dm-racewarn.ko`, если модуль ещё не загружен
- проверяет, что target `racewarn` появился в `dmsetup targets`

## Выгрузка модуля из ядра

```bash
sudo bash scripts/unload-racewarn.sh
```

## Тестирование

Тест:

```bash
sudo bash tests/test-racewarn.sh
```

Скрипт:

- создаёт временный backend-файл `/tmp/racewarn.img`
- привязывает его к `loop`-устройству
- создаёт `dm`-устройство `my0`
- запускает пересекающиеся запросы
- показывает `dmsetup status my0`
- показывает последние сообщения из `dmesg`
- удаляет созданные временные объекты

Пример конфликта, который должен быть обнаружен:

```bash
dd oflag=direct if=/dev/urandom of=/dev/mapper/my0 bs=32k count=1 seek=4 &
dd oflag=direct if=/dev/urandom of=/dev/mapper/my0 bs=8k count=1 seek=17
```

Логика здесь такая:

- первый запрос записывает диапазон `[256, 320)`
- второй запрос записывает диапазон `[272, 288)`
- диапазоны пересекаются, поэтому target должен вывести предупреждение

## Правила обнаружения гонок

Target сообщает о нарушениях следующего контракта:

- если запись блока `X` ещё не завершена, никакая другая операция для `X` не должна быть отправлена на устройство
- если чтение блока `X` ещё не завершено, запись блока `X` не должна быть отправлена на устройство

Текущая реализация отслеживает запросы `REQ_OP_READ` и `REQ_OP_WRITE`.

## Пример вывода

Пример предупреждения в `dmesg`:

```text
device-mapper: racewarn: race (write-after-read): write [256,320) conflicts with read [256,264) on loop0 (total conflicts: 3)
```

## На что опирался

- Разобрался с моделью работы `device-mapper`.
- Посмотрел и почитал про LVM, узнал что он как раз использует `device-mapper`
- Посмотрел, как устроены стандартные target'ы `dm-zero` и `dm-linear`.
- Основным ориентиром по написанию кода и был `include/linux/device-mapper.h` именно он задаёт интерфейс для работы.
- Разобрался, что `device-mapper` использует `/dev/mapper/control` и взаимодействует с userspace через системный вызов `ioctl()`.
- Для хранения активных диапазонов в этой ветке использовал `interval_tree` структуру линукс (сначала думал делать свою, но оказалось что в ядре она есть!!!).
- Для синхронизации доступа к общей структуре использовал `spin_lock_irqsave()` / `spin_unlock_irqrestore()`.
- `spinlock` был выбран вместо `mutex'а`, потому что здесь защищается очень короткая критическая секция в I/O path: обход дерева активных запросов, вставка нового интервала и удаление завершённого. Для такой секции выгоднее активное ожидание на короткое время, чем блокировка, которая может усыпить поток и привести к дополнительным накладным расходам на планирование и переключение контекста.
- При оформлении `target'а` как отдельного модуля ориентировался на `dm-zero`. Для регистрации и выгрузки target-а использовал макрос `module_dm(racewarn)`.